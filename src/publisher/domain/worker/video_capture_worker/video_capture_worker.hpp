#pragma once

#include "publisher/contracts/capture/desktop_capture_backend.hpp"
#include "publisher/domain/timing/frame_scheduler.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace semilive::publisher::domain {

struct VideoCaptureSessionConfig {
    contracts::capture::DesktopCaptureConfig capture;
    SessionTimeline timeline;
    FrameRate frame_rate{30, 1};
    std::chrono::milliseconds recovery_timeout{5000};
};

struct VideoCaptureStarted {
    contracts::capture::DesktopCaptureInfo source;
    std::chrono::steady_clock::time_point track_start;
    MediaTime first_presentation_time{};
};

enum class VideoCaptureWorkerState : std::uint8_t {
    Idle,
    Starting,
    Running,
    Stopping,
    Failed,
};

enum class VideoCaptureWorkerOperation : std::uint8_t {
    Control,
    ThreadInitialization,
    Configure,
    OpenBackend,
    Schedule,
    Capture,
    Recovery,
    Publish,
    Internal,
};

struct VideoCaptureWorkerIssue {
    VideoCaptureWorkerOperation operation = VideoCaptureWorkerOperation::Internal;
    std::optional<contracts::capture::DesktopCaptureIssue> capture_issue;
    std::string message;
};

struct VideoCaptureWorkerStats {
    std::uint64_t processed_ticks = 0;
    std::uint64_t scheduler_skipped_ticks = 0;
    std::uint64_t new_desktop_images = 0;
    std::uint64_t no_change_observations = 0;
    std::uint64_t missing_initial_frames = 0;
    std::uint64_t published_frames = 0;
    std::uint64_t repeated_frames = 0;
    std::uint64_t store_replaced_frames = 0;
    std::uint64_t temporarily_unavailable_observations = 0;
    std::uint64_t recovery_episodes = 0;
    std::uint64_t successful_recoveries = 0;
    std::uint64_t capture_calls = 0;
    std::chrono::nanoseconds total_capture_time{};
    std::chrono::nanoseconds maximum_capture_time{};
    std::uint64_t new_image_bytes = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint64_t source_size_changes = 0;
    std::uint64_t fatal_failures = 0;
};

class VideoCaptureWorker {
public:
    virtual ~VideoCaptureWorker() = default;

    VideoCaptureWorker(const VideoCaptureWorker&) = delete;
    VideoCaptureWorker& operator=(const VideoCaptureWorker&) = delete;
    VideoCaptureWorker(VideoCaptureWorker&&) = delete;
    VideoCaptureWorker& operator=(VideoCaptureWorker&&) = delete;

    [[nodiscard]] virtual std::expected<VideoCaptureStarted, VideoCaptureWorkerIssue>
    start(VideoCaptureSessionConfig config) = 0;

    virtual void stop() = 0;

    [[nodiscard]] virtual VideoCaptureWorkerState state() const noexcept = 0;
    [[nodiscard]] virtual VideoCaptureWorkerStats stats() const noexcept = 0;

protected:
    VideoCaptureWorker() = default;
};

}  // namespace semilive::publisher::domain
