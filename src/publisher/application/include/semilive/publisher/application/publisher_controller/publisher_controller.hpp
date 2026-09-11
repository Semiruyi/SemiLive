#pragma once

#include <semilive/publisher/domain/resource/captured_video_frame_store/captured_video_frame_store_control.hpp>
#include <semilive/publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue_control.hpp>
#include <semilive/publisher/domain/timing/session_timeline.hpp>
#include <semilive/publisher/domain/worker/video_capture_worker/video_capture_worker.hpp>
#include <semilive/publisher/domain/worker/video_encoder_worker/video_encoder_worker.hpp>
#include <semilive/publisher/domain/worker/video_output_worker/video_output_worker.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>

namespace semilive::publisher::application {

struct PublisherVideoSessionPlan {
    contracts::capture::DesktopCaptureConfig capture;
    std::chrono::milliseconds recovery_timeout{5000};
    contracts::encoder::VideoEncoderConfig encoder;
};

struct PublisherVideoPipeline {
    domain::VideoCaptureWorker& capture_worker;
    domain::VideoEncoderWorker& encoder_worker;
    domain::VideoOutputWorker& output_worker;
    domain::CapturedVideoFrameStoreControl& frame_store;
    domain::EncodedVideoAccessUnitQueueControl& access_unit_queue;
};

enum class PublisherControllerState : std::uint8_t {
    Idle,
    Starting,
    Running,
    Stopping,
    Failed,
};

enum class PublisherControllerOperation : std::uint8_t {
    Control,
    StartOutput,
    StartEncoder,
    StartCapture,
    VideoCaptureFailed,
    VideoEncoderFailed,
    VideoOutputFailed,
    StopCapture,
    DrainEncoder,
    DrainOutput,
    AbortPipeline,
    ClearResources,
    Internal,
};

using PublisherFailureDetail =
    std::variant<std::monostate,
                 domain::VideoCaptureWorkerIssue,
                 domain::VideoEncoderWorkerIssue,
                 domain::VideoOutputWorkerIssue>;

struct PublisherControllerIssue {
    PublisherControllerOperation operation =
        PublisherControllerOperation::Internal;
    PublisherFailureDetail detail;
    std::string message;
};

struct PublisherStarted {
    std::uint64_t session_id = 0;
    domain::SessionTimeline timeline{
        domain::SessionTimeline::Clock::time_point{}};
    domain::VideoCaptureStarted capture;
    domain::VideoEncoderStarted encoder;
    domain::VideoOutputStarted output;
};

struct PublisherStopped {
    std::uint64_t session_id = 0;
    std::size_t cleared_frames = 0;
    std::size_t cleared_access_units = 0;
};

struct PublisherControllerStats {
    PublisherControllerState state = PublisherControllerState::Idle;
    std::uint64_t session_id = 0;
    std::uint64_t started_sessions = 0;
    std::uint64_t completed_sessions = 0;
    std::uint64_t failed_sessions = 0;
    std::uint64_t start_failures = 0;
    std::uint64_t cleared_frames = 0;
    std::uint64_t cleared_access_units = 0;
    std::size_t frame_store_size = 0;
    std::size_t frame_store_peak_size = 0;
    std::size_t access_unit_queue_size = 0;
    std::size_t access_unit_queue_peak_size = 0;
    domain::VideoCaptureWorkerStats capture;
    domain::VideoEncoderWorkerStats encoder;
    domain::VideoOutputWorkerStats output;
    std::optional<PublisherControllerIssue> last_issue;
};

enum class PublisherWaitStatus : std::uint8_t {
    Timeout,
    Idle,
    Failed,
};

struct PublisherWaitResult {
    PublisherWaitStatus status = PublisherWaitStatus::Timeout;
    std::optional<PublisherControllerIssue> issue;
};

using PublisherStartResult =
    std::expected<PublisherStarted, PublisherControllerIssue>;
using PublisherStopResult =
    std::expected<PublisherStopped, PublisherControllerIssue>;

class PublisherController {
public:
    virtual ~PublisherController() = default;

    PublisherController(const PublisherController&) = delete;
    PublisherController& operator=(const PublisherController&) = delete;
    PublisherController(PublisherController&&) = delete;
    PublisherController& operator=(PublisherController&&) = delete;

    [[nodiscard]] virtual PublisherStartResult start_publishing() = 0;
    [[nodiscard]] virtual PublisherStopResult stop_publishing() = 0;

    [[nodiscard]] virtual PublisherControllerState state() const noexcept = 0;
    [[nodiscard]] virtual PublisherControllerStats stats() const noexcept = 0;
    [[nodiscard]] virtual PublisherWaitResult wait_for_terminal_for(
        std::chrono::milliseconds timeout) = 0;

protected:
    PublisherController() = default;
};

}  // namespace semilive::publisher::application
