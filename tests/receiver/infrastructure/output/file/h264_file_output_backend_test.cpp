#include <semilive/receiver/infrastructure/output/file/h264_file_output_backend.hpp>

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
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace contracts = semilive::receiver::contracts::output;
namespace model = semilive::receiver::model;

using semilive::receiver::infra::output::H264FileOutputBackend;

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
                ("semilive_receiver_h264_output_" + std::to_string(nonce) +
                 ".h264");
    }

    ~TemporaryOutputFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryOutputFile(const TemporaryOutputFile&) = delete;
    TemporaryOutputFile& operator=(const TemporaryOutputFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] model::TimedH264AccessUnit access_unit(
    std::vector<std::byte> annex_b,
    const std::uint32_t rtp_timestamp,
    const bool discontinuity_before = false) {
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
    return {std::move(unit), 33ms, rtp_timestamp, discontinuity_before};
}

[[nodiscard]] std::vector<std::byte> read_bytes(
    const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    require(input.is_open(), "output file must be readable");
    const std::vector<char> chars{std::istreambuf_iterator<char>{input},
                                  std::istreambuf_iterator<char>{}};

    std::vector<std::byte> bytes;
    bytes.reserve(chars.size());
    for (const char value : chars) {
        bytes.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return bytes;
}

void writes_annex_b_access_units_in_order() {
    TemporaryOutputFile output_file;
    H264FileOutputBackend backend{output_file.path()};
    const std::vector<std::byte> first{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x67}};
    const std::vector<std::byte> second{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x65}, std::byte{0x7f}};

    const auto opened = backend.open();
    const auto first_receipt = backend.submit(access_unit(first, 90'000, true));
    const auto second_receipt = backend.submit(access_unit(second, 93'000));
    backend.close(contracts::LiveVideoOutputCloseMode::Normal);
    backend.close(contracts::LiveVideoOutputCloseMode::Normal);

    require(opened && opened->output_name == output_file.path().string(),
            "open must report the configured output path");
    require(first_receipt &&
                first_receipt->status ==
                    contracts::LiveVideoSubmitStatus::Accepted &&
                first_receipt->accepted_bytes == first.size(),
            "file output must accept and account for the first access unit");
    require(second_receipt &&
                second_receipt->status ==
                    contracts::LiveVideoSubmitStatus::Accepted &&
                second_receipt->accepted_bytes == second.size(),
            "file output must accept and account for the second access unit");

    auto expected = first;
    expected.insert(expected.end(), second.begin(), second.end());
    require(read_bytes(output_file.path()) == expected,
            "file bytes must be the exact ordered Annex-B concatenation");
}

void reopen_truncates_the_previous_session() {
    TemporaryOutputFile output_file;
    H264FileOutputBackend backend{output_file.path()};
    const std::vector<std::byte> previous{std::byte{0x01}, std::byte{0x02}};
    const std::vector<std::byte> replacement{std::byte{0x03}};

    require(backend.open().has_value(), "first session must open");
    require(backend.submit(access_unit(previous, 1)).has_value(),
            "first session must write");
    backend.close(contracts::LiveVideoOutputCloseMode::Abort);

    require(backend.open().has_value(), "closed backend must reopen");
    require(backend.submit(access_unit(replacement, 2)).has_value(),
            "reopened backend must write");
    backend.close(contracts::LiveVideoOutputCloseMode::Normal);

    require(read_bytes(output_file.path()) == replacement,
            "reopening must truncate bytes from the previous session");
}

void lifecycle_and_input_errors_are_structured() {
    TemporaryOutputFile output_file;
    H264FileOutputBackend backend{output_file.path()};

    const auto submit_while_closed =
        backend.submit(access_unit({std::byte{0x01}}, 1));
    require(!submit_while_closed &&
                submit_while_closed.error().operation ==
                    contracts::LiveVideoOutputOperation::State,
            "submit while closed must report a state issue");

    require(backend.open().has_value(), "backend must open for lifecycle test");
    const auto duplicate_open = backend.open();
    require(!duplicate_open &&
                duplicate_open.error().operation ==
                    contracts::LiveVideoOutputOperation::State,
            "duplicate open must report a state issue");

    const auto empty_result = backend.submit(access_unit({}, 2));
    require(!empty_result &&
                empty_result.error().operation ==
                    contracts::LiveVideoOutputOperation::Submit,
            "empty access unit must report a submit issue");
    const auto submit_after_failure =
        backend.submit(access_unit({std::byte{0x02}}, 3));
    require(!submit_after_failure &&
                submit_after_failure.error().operation ==
                    contracts::LiveVideoOutputOperation::State,
            "failed session must require close before reuse");

    backend.close(contracts::LiveVideoOutputCloseMode::Abort);
    require(backend.open().has_value(),
            "close must reset a failed backend for a new session");
    backend.close(contracts::LiveVideoOutputCloseMode::Normal);
}

void invalid_paths_fail_without_creating_parents() {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    const auto missing_parent = std::filesystem::current_path() /
                                ("semilive_receiver_missing_output_" +
                                 std::to_string(nonce));
    const auto output_path = missing_parent / "video.h264";
    H264FileOutputBackend backend{output_path};

    const auto opened = backend.open();
    require(!opened &&
                opened.error().operation ==
                    contracts::LiveVideoOutputOperation::Open,
            "a missing parent directory must report an open issue");
    require(!std::filesystem::exists(missing_parent),
            "file backend must not create missing parent directories");
    backend.close(contracts::LiveVideoOutputCloseMode::Abort);

    H264FileOutputBackend empty_path_backend{std::filesystem::path{}};
    const auto empty_path = empty_path_backend.open();
    require(!empty_path &&
                empty_path.error().operation ==
                    contracts::LiveVideoOutputOperation::Open,
            "empty output path must report an open issue");
}

}  // namespace

int main() {
    try {
        writes_annex_b_access_units_in_order();
        reopen_truncates_the_previous_session();
        lifecycle_and_input_errors_are_structured();
        invalid_paths_fail_without_creating_parents();
    } catch (const std::exception& error) {
        std::cerr << "receiver H.264 file output backend test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "receiver H.264 file output backend tests passed\n";
    return EXIT_SUCCESS;
}
