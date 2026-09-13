#pragma once

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
    RunSession,
    StopSession,
    Internal,
};

struct VideoReceiveWorkerIssue {
    VideoReceiveWorkerOperation operation =
        VideoReceiveWorkerOperation::Internal;
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
