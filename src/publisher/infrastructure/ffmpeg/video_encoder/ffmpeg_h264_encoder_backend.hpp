#pragma once

#include <semilive/publisher/contracts/encoder/video_encoder_backend.hpp>
#include "publisher/infrastructure/ffmpeg/ffmpeg_raii.hpp"
#include "publisher/infrastructure/ffmpeg/video_encoder/ffmpeg_h264_encoder.hpp"
#include "publisher/infrastructure/ffmpeg/video_encoder/sws_frame_converter.hpp"
#include <semilive/publisher/model/media_clock.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <vector>

namespace semilive::publisher::infra::ffmpeg {

class FfmpegH264EncoderBackend final
    : public contracts::encoder::VideoEncoderBackend {
public:
    FfmpegH264EncoderBackend() = default;
    ~FfmpegH264EncoderBackend() override;

    [[nodiscard]] contracts::encoder::VideoEncoderOpenResult open(
        const contracts::encoder::VideoEncoderConfig& config) override;
    [[nodiscard]] contracts::encoder::VideoEncodeResult encode(
        const model::CapturedVideoFrame& frame) override;
    [[nodiscard]] contracts::encoder::VideoEncodeResult flush() override;
    void close() noexcept override;

private:
    enum class State : std::uint8_t {
        Closed,
        Open,
        Flushed,
        Failed,
    };

    struct FrameMetadata {
        model::MediaTime presentation_time{};
        std::uint64_t source_sequence = 0;
        std::chrono::steady_clock::time_point captured_at{};
    };

    using AccessUnitResult =
        std::expected<std::vector<model::EncodedVideoAccessUnit>,
                      contracts::encoder::VideoEncoderIssue>;

    [[nodiscard]] AccessUnitResult make_access_units(
        std::vector<FfmpegEncodedPacket> packets);
    [[nodiscard]] contracts::encoder::VideoEncodeResult fail(
        contracts::encoder::VideoEncoderIssue issue);

    SwsFrameConverter converter_;
    FfmpegH264Encoder encoder_;
    AvFramePtr encoder_frame_;
    model::VideoDimensions output_{};
    std::uint32_t maximum_delayed_frames_ = 0;
    std::map<model::MediaClockTicks, FrameMetadata> metadata_by_pts_;
    std::optional<model::MediaClockTicks> last_submitted_ticks_;
    std::optional<model::MediaClockTicks> last_emitted_ticks_;
    State state_ = State::Closed;
};

}  // namespace semilive::publisher::infra::ffmpeg
