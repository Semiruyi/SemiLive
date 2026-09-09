#pragma once

#include <semilive/publisher/contracts/encoder/video_encoder_backend.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace semilive::publisher::domain {

struct VideoEncoderSessionConfig {
    contracts::encoder::VideoEncoderConfig encoder;
};

struct VideoEncoderStarted {
    contracts::encoder::VideoEncoderInfo encoder;
};

enum class VideoEncoderStopMode : std::uint8_t {
    Drain,
    Abort,
};

enum class VideoEncoderWorkerState : std::uint8_t {
    Idle,
    Starting,
    Running,
    Draining,
    Failed,
};

enum class VideoEncoderWorkerOperation : std::uint8_t {
    Control,
    ThreadInitialization,
    OpenBackend,
    ConsumeFrame,
    Encode,
    PublishAccessUnit,
    Flush,
    Internal,
};

struct VideoEncoderWorkerIssue {
    VideoEncoderWorkerOperation operation = VideoEncoderWorkerOperation::Internal;
    std::optional<contracts::encoder::VideoEncoderIssue> encoder_issue;
    std::string message;
};

struct VideoEncoderWorkerStats {
    std::optional<contracts::encoder::VideoEncoderInfo> encoder;
    std::uint64_t consumed_frames = 0;
    std::uint64_t input_sequence_gaps = 0;
    std::uint64_t missing_input_frames = 0;
    std::uint64_t zero_output_calls = 0;
    std::uint64_t single_output_calls = 0;
    std::uint64_t multiple_output_calls = 0;
    std::uint64_t produced_access_units = 0;
    std::uint64_t submitted_access_units = 0;
    std::uint64_t pending_access_units = 0;
    std::uint64_t key_frames = 0;
    std::uint64_t encoded_bytes = 0;
    std::uint64_t backend_calls = 0;
    std::chrono::nanoseconds total_backend_time{};
    std::chrono::nanoseconds maximum_backend_time{};
    std::chrono::nanoseconds total_preprocessing_time{};
    std::chrono::nanoseconds maximum_preprocessing_time{};
    std::chrono::nanoseconds total_codec_time{};
    std::chrono::nanoseconds maximum_codec_time{};
    std::uint64_t access_unit_queue_full_events = 0;
    std::chrono::nanoseconds total_backpressure_time{};
    std::chrono::nanoseconds maximum_backpressure_time{};
    std::uint64_t capture_to_encode_samples = 0;
    std::chrono::nanoseconds total_capture_to_encode_time{};
    std::chrono::nanoseconds maximum_capture_to_encode_time{};
    std::uint64_t drained_input_frames = 0;
    std::uint64_t flushed_access_units = 0;
    std::uint64_t aborted_pending_access_units = 0;
    std::uint64_t fatal_failures = 0;
    std::optional<VideoEncoderWorkerIssue> last_issue;
};

class VideoEncoderWorker {
public:
    virtual ~VideoEncoderWorker() = default;

    VideoEncoderWorker(const VideoEncoderWorker&) = delete;
    VideoEncoderWorker& operator=(const VideoEncoderWorker&) = delete;
    VideoEncoderWorker(VideoEncoderWorker&&) = delete;
    VideoEncoderWorker& operator=(VideoEncoderWorker&&) = delete;

    [[nodiscard]] virtual std::expected<VideoEncoderStarted,
                                                VideoEncoderWorkerIssue>
    start(VideoEncoderSessionConfig config) = 0;
    virtual void stop(VideoEncoderStopMode mode) = 0;

    [[nodiscard]] virtual VideoEncoderWorkerState state() const noexcept = 0;
    [[nodiscard]] virtual VideoEncoderWorkerStats stats() const noexcept = 0;

protected:
    VideoEncoderWorker() = default;
};

}  // namespace semilive::publisher::domain
