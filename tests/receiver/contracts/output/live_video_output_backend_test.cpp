#include <semilive/receiver/contracts/output/live_video_output_backend.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace output = semilive::receiver::contracts::output;
namespace model = semilive::receiver::model;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

[[nodiscard]] model::TimedH264AccessUnit access_unit() {
    return {
        model::H264AccessUnit{
            std::vector<std::byte>{std::byte{0x00}, std::byte{0x00},
                                   std::byte{0x00}, std::byte{0x01},
                                   std::byte{0x65}},
            90'000,
            10,
            12,
            model::H264AccessUnit::Clock::time_point{5ms},
            1,
            true,
            true,
            true,
            true},
        1s,
        0x1'0000'0000ULL,
        true};
}

class RecordingLiveVideoOutput final
    : public output::LiveVideoOutputBackend {
public:
    [[nodiscard]] output::LiveVideoOutputOpenResult open() override {
        opened = true;
        return output::LiveVideoOutputInfo{"memory://live-video"};
    }

    [[nodiscard]] output::LiveVideoOutputSubmitResult submit(
        model::TimedH264AccessUnit access_unit) override {
        submitted = std::move(access_unit);
        return next_receipt;
    }

    void close(const output::LiveVideoOutputCloseMode mode) noexcept override {
        close_mode = mode;
    }

    output::LiveVideoSubmitReceipt next_receipt{
        output::LiveVideoSubmitStatus::Accepted, 5};
    std::optional<model::TimedH264AccessUnit> submitted;
    std::optional<output::LiveVideoOutputCloseMode> close_mode;
    bool opened = false;
};

static_assert(!std::is_copy_constructible_v<RecordingLiveVideoOutput>);
static_assert(!std::is_move_constructible_v<RecordingLiveVideoOutput>);

void transfers_complete_timed_access_units_and_close_mode() {
    RecordingLiveVideoOutput backend;
    const auto opened = backend.open();
    require(opened && opened->output_name == "memory://live-video",
            "open must preserve output identity");

    const auto submitted = backend.submit(access_unit());
    require(submitted &&
                submitted->status == output::LiveVideoSubmitStatus::Accepted &&
                submitted->accepted_bytes == 5,
            "submit must preserve its acceptance receipt");
    require(backend.submitted &&
                backend.submitted->presentation_time() == 1s &&
                backend.submitted->extended_rtp_timestamp() ==
                    0x1'0000'0000ULL &&
                backend.submitted->discontinuity_before() &&
                backend.submitted->access_unit().annex_b().size() == 5,
            "submit must transfer bytes, timing and recovery metadata");

    backend.close(output::LiveVideoOutputCloseMode::Normal);
    require(backend.close_mode == output::LiveVideoOutputCloseMode::Normal,
            "normal close intent must reach the backend");
}

void distinguishes_backpressure_drop_from_fatal_output_failure() {
    RecordingLiveVideoOutput backend;
    backend.next_receipt = {
        output::LiveVideoSubmitStatus::DroppedBackpressure, 0};
    const auto dropped = backend.submit(access_unit());
    require(dropped &&
                dropped->status ==
                    output::LiveVideoSubmitStatus::DroppedBackpressure,
            "temporary saturation must remain a normal drop result");

    class FailingOutput final : public output::LiveVideoOutputBackend {
    public:
        [[nodiscard]] output::LiveVideoOutputOpenResult open() override {
            return output::LiveVideoOutputInfo{};
        }

        [[nodiscard]] output::LiveVideoOutputSubmitResult submit(
            model::TimedH264AccessUnit) override {
            return std::unexpected{output::LiveVideoOutputIssue{
                output::LiveVideoOutputOperation::Submit,
                -28,
                "output failed"}};
        }

        void close(output::LiveVideoOutputCloseMode) noexcept override {}
    } failing;

    const auto failed = failing.submit(access_unit());
    require(!failed &&
                failed.error().operation ==
                    output::LiveVideoOutputOperation::Submit &&
                failed.error().native_code == -28 &&
                failed.error().message == "output failed",
            "fatal submit failures must retain structured issue details");
}

}  // namespace

int main() {
    try {
        transfers_complete_timed_access_units_and_close_mode();
        distinguishes_backpressure_drop_from_fatal_output_failure();
    } catch (const std::exception&) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
