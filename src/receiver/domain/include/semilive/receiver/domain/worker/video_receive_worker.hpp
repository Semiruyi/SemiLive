#pragma once

#include <semilive/receiver/contracts/network/datagram_source_backend.hpp>
#include <semilive/receiver/contracts/output/live_video_output_backend.hpp>
#include <semilive/receiver/domain/pipeline/h264_rtp_receive_pipeline.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace semilive::receiver::domain {

enum class VideoReceiveWorkerState : std::uint8_t {
    Idle,
    Starting,
    Running,
    Stopping,
    Failed,
};

enum class VideoReceiveWorkerOperation : std::uint8_t {
    Control,
    StartSession,
    OpenOutput,
    OpenInput,
    ReceiveInput,
    SubmitOutput,
    RunSession,
    StopSession,
    Internal,
};

struct VideoReceiveWorkerIssue {
    VideoReceiveWorkerOperation operation =
        VideoReceiveWorkerOperation::Internal;
    std::optional<contracts::network::DatagramSourceIssue> input_issue;
    std::optional<contracts::output::LiveVideoOutputIssue> output_issue;
    std::string message;
};

struct VideoReceiveStarted {
    std::uint64_t session_id = 0;
};

struct VideoReceiveStopped {
    std::uint64_t session_id = 0;
};

struct VideoReceiveWorkerStats {
    VideoReceiveWorkerState state = VideoReceiveWorkerState::Idle;
    std::uint64_t session_id = 0;
    std::uint64_t started_sessions = 0;
    std::uint64_t completed_sessions = 0;
    std::uint64_t failed_sessions = 0;
    std::uint64_t start_failures = 0;
    std::uint64_t received_datagrams = 0;
    std::uint64_t received_bytes = 0;
    std::uint64_t receive_timeouts = 0;
    std::uint64_t submitted_access_units = 0;
    std::uint64_t submitted_bytes = 0;
    std::uint64_t backpressure_drops = 0;
    std::uint64_t discarded_after_backpressure = 0;
    std::chrono::nanoseconds session_duration{};
    std::optional<std::chrono::nanoseconds> first_output_delay;
    std::optional<std::chrono::nanoseconds> maximum_output_gap;
    std::optional<std::chrono::nanoseconds> terminal_output_gap;
    std::uint64_t output_stall_events = 0;
    std::chrono::nanoseconds output_stall_excess_total{};
    std::optional<contracts::network::DatagramSourceInfo> input;
    std::optional<contracts::output::LiveVideoOutputInfo> output;
    H264RtpReceivePipelineStats pipeline;
};

enum class VideoReceiveWaitStatus : std::uint8_t {
    Timeout,
    Idle,
    Failed,
};

struct VideoReceiveWaitResult {
    VideoReceiveWaitStatus status = VideoReceiveWaitStatus::Timeout;
    std::optional<VideoReceiveWorkerIssue> issue;
};

using VideoReceiveStartResult =
    std::expected<VideoReceiveStarted, VideoReceiveWorkerIssue>;
using VideoReceiveStopResult =
    std::expected<VideoReceiveStopped, VideoReceiveWorkerIssue>;

class VideoReceiveWorker {
public:
    virtual ~VideoReceiveWorker() = default;

    VideoReceiveWorker(const VideoReceiveWorker&) = delete;
    VideoReceiveWorker& operator=(const VideoReceiveWorker&) = delete;
    VideoReceiveWorker(VideoReceiveWorker&&) = delete;
    VideoReceiveWorker& operator=(VideoReceiveWorker&&) = delete;

    [[nodiscard]] virtual VideoReceiveStartResult start() = 0;
    [[nodiscard]] virtual VideoReceiveStopResult stop() = 0;

    [[nodiscard]] virtual VideoReceiveWorkerState state() const noexcept = 0;
    [[nodiscard]] virtual VideoReceiveWorkerStats stats() const noexcept = 0;
    [[nodiscard]] virtual VideoReceiveWaitResult wait_for_terminal_for(
        std::chrono::milliseconds timeout) = 0;

protected:
    VideoReceiveWorker() = default;
};

}  // namespace semilive::receiver::domain
