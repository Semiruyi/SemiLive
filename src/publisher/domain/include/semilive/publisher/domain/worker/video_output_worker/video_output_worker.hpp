#pragma once

#include <semilive/publisher/contracts/output/video_access_unit_output_backend.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace semilive::publisher::domain {

struct VideoOutputStarted {
    contracts::output::VideoOutputInfo output;
};

enum class VideoOutputStopMode : std::uint8_t {
    Drain,
    Abort,
};

enum class VideoOutputWorkerState : std::uint8_t {
    Idle,
    Starting,
    Running,
    Draining,
    Failed,
};

enum class VideoOutputWorkerOperation : std::uint8_t {
    Control,
    ThreadInitialization,
    OpenBackend,
    ConsumeAccessUnit,
    ValidateAccessUnit,
    Output,
    Flush,
    Internal,
};

struct VideoOutputWorkerIssue {
    VideoOutputWorkerOperation operation = VideoOutputWorkerOperation::Internal;
    std::optional<contracts::output::VideoOutputIssue> output_issue;
    std::string message;
};

struct VideoOutputWorkerStats {
    std::optional<contracts::output::VideoOutputInfo> output;
    std::uint64_t consumed_access_units = 0;
    std::uint64_t drained_access_units = 0;
    std::uint64_t key_frames = 0;
    std::uint64_t input_bytes = 0;
    std::uint64_t emitted_units = 0;
    std::uint64_t emitted_bytes = 0;
    std::uint64_t backend_calls = 0;
    std::uint64_t flush_calls = 0;
    std::chrono::nanoseconds total_backend_time{};
    std::chrono::nanoseconds maximum_backend_time{};
    std::uint64_t capture_to_output_samples = 0;
    std::chrono::nanoseconds total_capture_to_output_time{};
    std::chrono::nanoseconds maximum_capture_to_output_time{};
    std::uint64_t fatal_failures = 0;
    std::optional<VideoOutputWorkerIssue> last_issue;
};

class VideoOutputWorker {
public:
    virtual ~VideoOutputWorker() = default;

    VideoOutputWorker(const VideoOutputWorker&) = delete;
    VideoOutputWorker& operator=(const VideoOutputWorker&) = delete;
    VideoOutputWorker(VideoOutputWorker&&) = delete;
    VideoOutputWorker& operator=(VideoOutputWorker&&) = delete;

    [[nodiscard]] virtual std::expected<VideoOutputStarted,
                                                VideoOutputWorkerIssue>
    start() = 0;
    virtual void stop(VideoOutputStopMode mode) = 0;

    [[nodiscard]] virtual VideoOutputWorkerState state() const noexcept = 0;
    [[nodiscard]] virtual VideoOutputWorkerStats stats() const noexcept = 0;

protected:
    VideoOutputWorker() = default;
};

}  // namespace semilive::publisher::domain
