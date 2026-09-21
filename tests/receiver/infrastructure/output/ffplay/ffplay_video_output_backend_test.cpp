#include <semilive/receiver/infrastructure/output/ffplay/ffplay_video_output_backend.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace contracts = semilive::receiver::contracts::output;
namespace model = semilive::receiver::model;

using semilive::receiver::infra::output::FfplayVideoOutputBackend;
using semilive::receiver::infra::output::FfplayVideoOutputConfig;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

class TemporaryOutputFile final {
public:
    TemporaryOutputFile() {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::current_path() /
                ("semilive_receiver_ffplay_output_" +
                 std::to_string(nonce) + ".h264");
    }

    ~TemporaryOutputFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] model::TimedH264AccessUnit access_unit(
    std::vector<std::byte> annex_b,
    const std::uint32_t rtp_timestamp) {
    const auto size = annex_b.size();
    model::H264AccessUnit unit{std::move(annex_b),
                              rtp_timestamp,
                              10,
                              12,
                              model::H264AccessUnit::Clock::now(),
                              size == 0U ? 0U : 1U,
                              size != 0U,
                              false,
                              false,
                              false};
    return {std::move(unit), 33ms, rtp_timestamp, false};
}

[[nodiscard]] std::vector<std::byte> read_bytes(
    const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    require(input.is_open(), "helper output must be readable");
    const std::vector<char> chars{std::istreambuf_iterator<char>{input},
                                  std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes;
    bytes.reserve(chars.size());
    for (const auto value : chars) {
        bytes.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return bytes;
}

[[nodiscard]] FfplayVideoOutputConfig helper_config(
    const std::filesystem::path& output_path) {
    FfplayVideoOutputConfig config;
    config.executable_path = SEMILIVE_FFPLAY_TEST_HELPER;
    config.arguments = {"--output", output_path.string()};
    return config;
}

void streams_annex_b_access_units_to_the_child_process() {
    TemporaryOutputFile output;
    FfplayVideoOutputBackend backend{helper_config(output.path())};
    const std::vector<std::byte> first{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x67}};
    const std::vector<std::byte> second{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x65}, std::byte{0x7f}};

    const auto opened = backend.open();
    const auto first_receipt = backend.submit(access_unit(first, 90'000));
    const auto second_receipt = backend.submit(access_unit(second, 93'000));
    backend.close(contracts::LiveVideoOutputCloseMode::Normal);

    require(opened && opened->output_name.starts_with("ffplay:"),
            "open must identify the ffplay output");
    require(first_receipt &&
                first_receipt->status ==
                    contracts::LiveVideoSubmitStatus::Accepted &&
                first_receipt->accepted_bytes == first.size(),
            "first access unit must be accepted in full");
    require(second_receipt &&
                second_receipt->status ==
                    contracts::LiveVideoSubmitStatus::Accepted &&
                second_receipt->accepted_bytes == second.size(),
            "second access unit must be accepted in full");

    auto expected = first;
    expected.insert(expected.end(), second.begin(), second.end());
    require(read_bytes(output.path()) == expected,
            "child stdin must receive exact ordered Annex-B bytes");
}

void bounded_queue_reports_backpressure_without_partial_acceptance() {
    TemporaryOutputFile output;
    auto config = helper_config(output.path());
    config.arguments.insert(config.arguments.begin(),
                            {"--initial-delay-ms", "750"});
    constexpr std::size_t large_size = 2U * 1024U * 1024U;
    config.maximum_buffered_access_units = 2;
    config.maximum_buffered_bytes = large_size;
    FfplayVideoOutputBackend backend{std::move(config)};

    require(backend.open().has_value(), "backpressure helper must open");
    const auto first = backend.submit(
        access_unit(std::vector<std::byte>(large_size, std::byte{0x01}), 1));
    const auto second =
        backend.submit(access_unit({std::byte{0x02}}, 2));
    require(first &&
                first->status == contracts::LiveVideoSubmitStatus::Accepted &&
                first->accepted_bytes == large_size,
            "the first large access unit must be accepted");
    require(second &&
                second->status ==
                    contracts::LiveVideoSubmitStatus::DroppedBackpressure &&
                second->accepted_bytes == 0,
            "a full byte budget must drop the complete next access unit");
    backend.close(contracts::LiveVideoOutputCloseMode::Abort);
}

void lifecycle_and_configuration_failures_are_structured() {
    TemporaryOutputFile output;
    auto config = helper_config(output.path());
    FfplayVideoOutputBackend backend{config};

    const auto closed_submit =
        backend.submit(access_unit({std::byte{0x01}}, 1));
    require(!closed_submit &&
                closed_submit.error().operation ==
                    contracts::LiveVideoOutputOperation::State,
            "submit while closed must report a state issue");
    require(backend.open().has_value(), "helper backend must open");
    const auto duplicate_open = backend.open();
    require(!duplicate_open &&
                duplicate_open.error().operation ==
                    contracts::LiveVideoOutputOperation::State,
            "duplicate open must report a state issue");
    const auto empty_submit = backend.submit(access_unit({}, 2));
    require(!empty_submit &&
                empty_submit.error().operation ==
                    contracts::LiveVideoOutputOperation::Submit,
            "empty access units must report a submit issue");
    backend.close(contracts::LiveVideoOutputCloseMode::Normal);
    require(backend.open().has_value(),
            "a normally closed backend must reopen");
    backend.close(contracts::LiveVideoOutputCloseMode::Abort);

    config.maximum_buffered_access_units = 0;
    FfplayVideoOutputBackend invalid_limits{std::move(config)};
    const auto invalid_open = invalid_limits.open();
    require(!invalid_open &&
                invalid_open.error().operation ==
                    contracts::LiveVideoOutputOperation::Open,
            "zero queue limits must fail during open");

    FfplayVideoOutputConfig missing_config;
    missing_config.executable_path =
        "semilive-ffplay-executable-that-does-not-exist";
    FfplayVideoOutputBackend missing_executable{std::move(missing_config)};
    const auto missing_open = missing_executable.open();
    require(!missing_open &&
                missing_open.error().operation ==
                    contracts::LiveVideoOutputOperation::Open,
            "a missing executable must report an open issue");
}

void child_exit_is_reported_on_a_later_submission() {
    TemporaryOutputFile output;
    auto config = helper_config(output.path());
    config.arguments = {"--exit-immediately"};
    FfplayVideoOutputBackend backend{std::move(config)};

    require(backend.open().has_value(), "exiting helper must start");
    const auto accepted =
        backend.submit(access_unit({std::byte{0x01}}, 1));
    require(accepted.has_value(),
            "ownership transfer may complete before an asynchronous failure");
    std::this_thread::sleep_for(250ms);
    auto observed =
        backend.submit(access_unit({std::byte{0x02}}, 2));
    if (observed) {
        std::this_thread::sleep_for(250ms);
        observed = backend.submit(access_unit({std::byte{0x03}}, 3));
    }
    require(!observed &&
                observed.error().operation ==
                    contracts::LiveVideoOutputOperation::Submit,
            "a closed child pipe must surface as a submit issue");
    backend.close(contracts::LiveVideoOutputCloseMode::Abort);
}

}  // namespace

int main() {
    try {
        streams_annex_b_access_units_to_the_child_process();
        bounded_queue_reports_backpressure_without_partial_acceptance();
        lifecycle_and_configuration_failures_are_structured();
        child_exit_is_reported_on_a_later_submission();
    } catch (const std::exception& error) {
        std::cerr << "receiver ffplay output backend test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "receiver ffplay output backend tests passed\n";
    return EXIT_SUCCESS;
}
