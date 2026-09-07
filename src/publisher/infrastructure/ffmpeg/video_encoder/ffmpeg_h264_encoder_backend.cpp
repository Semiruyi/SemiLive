#include "publisher/infrastructure/ffmpeg/video_encoder/ffmpeg_h264_encoder_backend.hpp"

#include <semilive/publisher/model/media_clock.hpp>
#include <semilive/publisher/model/video/video_placement_calculator.hpp>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace semilive::publisher::infra::ffmpeg {
namespace {

using contracts::encoder::VideoEncodeBatch;
using contracts::encoder::VideoEncoderIssue;
using contracts::encoder::VideoEncoderOperation;

constexpr model::MediaClockRate kEncoderClockRate{90'000};
constexpr std::uint32_t kMaximumDelayedFrames = 1;

VideoEncoderIssue issue(const VideoEncoderOperation operation,
                        const std::int64_t native_code,
                        std::string message) {
    return VideoEncoderIssue{operation, native_code, std::move(message)};
}

VideoEncoderIssue ffmpeg_issue(const VideoEncoderOperation operation,
                               const int native_code,
                               const std::string_view context) {
    char detail[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(native_code, detail, sizeof(detail));
    return issue(operation, native_code,
                 std::string{context} + ": " + detail);
}

std::optional<VideoEncoderIssue> validate_input_image(
    const model::CapturedVideoFrame& frame) {
    if (!frame.image) {
        return issue(VideoEncoderOperation::ValidateInput, 0,
                     "captured video frame must contain a BGRA image");
    }

    const auto& image = *frame.image;
    if (image.width == 0 || image.height == 0 || image.stride == 0) {
        return issue(VideoEncoderOperation::ValidateInput, 0,
                     "BGRA input dimensions and stride must be non-zero");
    }
    if (image.width > std::numeric_limits<std::uint32_t>::max() / 4U ||
        image.stride < image.width * 4U) {
        return issue(VideoEncoderOperation::ValidateInput, 0,
                     "BGRA input stride is smaller than one pixel row");
    }
    if (image.width > static_cast<std::uint32_t>(INT_MAX) ||
        image.height > static_cast<std::uint32_t>(INT_MAX) ||
        image.stride > static_cast<std::uint32_t>(INT_MAX)) {
        return issue(VideoEncoderOperation::ValidateInput, 0,
                     "BGRA input layout exceeds FFmpeg integer limits");
    }

    const auto height = static_cast<std::size_t>(image.height);
    const auto stride = static_cast<std::size_t>(image.stride);
    if (height > std::numeric_limits<std::size_t>::max() / stride ||
        image.bgra.size() < height * stride) {
        return issue(VideoEncoderOperation::ValidateInput, 0,
                     "BGRA input buffer does not cover its declared layout");
    }
    return std::nullopt;
}

std::string clock_conversion_error_message(
    const model::MediaTimeConversionError error) {
    switch (error) {
    case model::MediaTimeConversionError::InvalidClockRate:
        return "video encoder clock rate must be positive";
    case model::MediaTimeConversionError::NegativeTime:
        return "video presentation time must not be negative";
    case model::MediaTimeConversionError::Overflow:
        return "video presentation time exceeds the media clock range";
    }
    return "failed to convert video presentation time to media clock ticks";
}

std::expected<model::MediaClockTicks, VideoEncoderIssue> encoder_ticks(
    const model::MediaTime presentation_time,
    const std::optional<model::MediaClockTicks> last_submitted_ticks) {
    auto ticks = model::media_time_to_clock_ticks(
        presentation_time, kEncoderClockRate);
    if (!ticks) {
        return std::unexpected{issue(
            VideoEncoderOperation::ValidateInput, 0,
            clock_conversion_error_message(ticks.error()))};
    }
    if (ticks->value > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected{issue(
            VideoEncoderOperation::ValidateInput, 0,
            "video presentation time exceeds the encoder PTS range")};
    }
    if (last_submitted_ticks && *ticks <= *last_submitted_ticks) {
        return std::unexpected{issue(
            VideoEncoderOperation::ValidateInput, 0,
            "mapped 90 kHz video PTS must be strictly increasing")};
    }
    return *ticks;
}

std::string placement_error_message(const model::VideoPlacementError error) {
    switch (error) {
    case model::VideoPlacementError::EmptyInput:
        return "cannot calculate video placement for an empty input";
    case model::VideoPlacementError::InvalidOutput:
        return "cannot calculate video placement for an invalid output canvas";
    case model::VideoPlacementError::ScaledImageTooSmall:
        return "scaled video placement is too small for YUV420P";
    }
    return "failed to calculate video placement";
}

std::expected<AvFramePtr, VideoEncoderIssue> allocate_encoder_frame(
    const model::VideoDimensions dimensions) {
    AvFramePtr frame{av_frame_alloc()};
    if (!frame) {
        return std::unexpected{ffmpeg_issue(
            VideoEncoderOperation::Open, AVERROR(ENOMEM),
            "failed to allocate the encoder YUV420P frame")};
    }

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = static_cast<int>(dimensions.width);
    frame->height = static_cast<int>(dimensions.height);
    const int result = av_frame_get_buffer(frame.get(), 32);
    if (result < 0) {
        return std::unexpected{ffmpeg_issue(
            VideoEncoderOperation::Open, result,
            "failed to allocate the encoder YUV420P frame buffer")};
    }
    return frame;
}

}  // namespace

FfmpegH264EncoderBackend::~FfmpegH264EncoderBackend() {
    close();
}

contracts::encoder::VideoEncoderOpenResult FfmpegH264EncoderBackend::open(
    const contracts::encoder::VideoEncoderConfig& config) {
    if (state_ != State::Closed) {
        return std::unexpected{issue(
            VideoEncoderOperation::State, 0,
            "FFmpeg H.264 backend can only open from the closed state")};
    }

    auto opened = encoder_.open(config);
    if (!opened) {
        return std::unexpected{std::move(opened.error())};
    }
    if (opened->maximum_delayed_frames != kMaximumDelayedFrames) {
        close();
        return std::unexpected{issue(
            VideoEncoderOperation::Open, 0,
            "FFmpeg H.264 encoder reported an unsupported delay bound")};
    }

    auto configured = converter_.configure(opened->output);
    if (!configured) {
        auto error = std::move(configured.error());
        close();
        return std::unexpected{std::move(error)};
    }
    auto frame = allocate_encoder_frame(opened->output);
    if (!frame) {
        auto error = std::move(frame.error());
        close();
        return std::unexpected{std::move(error)};
    }

    encoder_frame_ = std::move(*frame);
    output_ = opened->output;
    maximum_delayed_frames_ = opened->maximum_delayed_frames;
    state_ = State::Open;
    return opened;
}

contracts::encoder::VideoEncodeResult FfmpegH264EncoderBackend::encode(
    const model::CapturedVideoFrame& frame) {
    if (state_ != State::Open) {
        return std::unexpected{issue(
            VideoEncoderOperation::State, 0,
            "FFmpeg H.264 backend must be open before encoding")};
    }
    if (auto validation_error = validate_input_image(frame)) {
        return fail(std::move(*validation_error));
    }
    auto ticks = encoder_ticks(frame.presentation_time,
                               last_submitted_ticks_);
    if (!ticks) {
        return fail(std::move(ticks.error()));
    }

    const auto preprocessing_started = std::chrono::steady_clock::now();
    auto placement = model::calculate_video_placement(
        model::VideoDimensions{frame.image->width, frame.image->height},
        output_);
    if (!placement) {
        return fail(issue(VideoEncoderOperation::CalculatePlacement, 0,
                          placement_error_message(placement.error())));
    }
    auto converted = converter_.convert(*frame.image, *placement,
                                        *encoder_frame_);
    if (!converted) {
        return fail(std::move(converted.error()));
    }
    encoder_frame_->pts = static_cast<std::int64_t>(ticks->value);
    const auto preprocessing_finished = std::chrono::steady_clock::now();

    const auto [metadata, inserted] = metadata_by_pts_.emplace(
        *ticks, FrameMetadata{frame.presentation_time, frame.sequence,
                              frame.captured_at});
    static_cast<void>(metadata);
    if (!inserted) {
        return fail(issue(VideoEncoderOperation::ValidateInput, 0,
                          "video PTS already has pending metadata"));
    }

    const auto codec_started = std::chrono::steady_clock::now();
    auto packets = encoder_.encode(*encoder_frame_);
    const auto codec_finished = std::chrono::steady_clock::now();
    if (!packets) {
        return fail(std::move(packets.error()));
    }
    last_submitted_ticks_ = *ticks;

    auto access_units = make_access_units(std::move(*packets));
    if (!access_units) {
        return fail(std::move(access_units.error()));
    }
    if (metadata_by_pts_.size() > maximum_delayed_frames_) {
        return fail(issue(
            VideoEncoderOperation::ReceivePacket, 0,
            "FFmpeg H.264 encoder exceeded its delayed-frame bound"));
    }

    VideoEncodeBatch batch;
    batch.access_units = std::move(*access_units);
    batch.preprocessing_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
        preprocessing_finished - preprocessing_started);
    batch.codec_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
        codec_finished - codec_started);
    return batch;
}

contracts::encoder::VideoEncodeResult FfmpegH264EncoderBackend::flush() {
    if (state_ != State::Open) {
        return std::unexpected{issue(
            VideoEncoderOperation::State, 0,
            "FFmpeg H.264 backend can only flush an open session once")};
    }

    const auto codec_started = std::chrono::steady_clock::now();
    auto packets = encoder_.flush();
    const auto codec_finished = std::chrono::steady_clock::now();
    if (!packets) {
        return fail(std::move(packets.error()));
    }

    auto access_units = make_access_units(std::move(*packets));
    if (!access_units) {
        return fail(std::move(access_units.error()));
    }
    if (!metadata_by_pts_.empty()) {
        return fail(issue(
            VideoEncoderOperation::Flush, 0,
            "FFmpeg H.264 flush ended with unmatched input metadata"));
    }

    VideoEncodeBatch batch;
    batch.access_units = std::move(*access_units);
    batch.codec_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
        codec_finished - codec_started);
    state_ = State::Flushed;
    return batch;
}

FfmpegH264EncoderBackend::AccessUnitResult
FfmpegH264EncoderBackend::make_access_units(
    std::vector<FfmpegEncodedPacket> packets) {
    std::vector<model::EncodedVideoAccessUnit> access_units;
    access_units.reserve(packets.size());

    for (auto& packet : packets) {
        if (packet.pts < 0) {
            return std::unexpected{issue(
                VideoEncoderOperation::ReceivePacket, 0,
                "encoded H.264 packet PTS must not be negative")};
        }
        const model::MediaClockTicks packet_ticks{
            static_cast<std::uint64_t>(packet.pts)};
        if (last_emitted_ticks_ && packet_ticks <= *last_emitted_ticks_) {
            return std::unexpected{issue(
                VideoEncoderOperation::ReceivePacket, 0,
                "encoded H.264 packet PTS must be strictly increasing")};
        }
        const auto metadata = metadata_by_pts_.find(packet_ticks);
        if (metadata == metadata_by_pts_.end()) {
            return std::unexpected{issue(
                VideoEncoderOperation::ReceivePacket, 0,
                "encoded H.264 packet PTS has no matching input metadata")};
        }

        const FrameMetadata frame_metadata = metadata->second;
        access_units.push_back(model::EncodedVideoAccessUnit{
            std::move(packet.annex_b),
            frame_metadata.presentation_time,
            packet.key_frame,
            frame_metadata.source_sequence,
            frame_metadata.captured_at,
        });
        metadata_by_pts_.erase(metadata);
        last_emitted_ticks_ = packet_ticks;
    }
    return access_units;
}

contracts::encoder::VideoEncodeResult FfmpegH264EncoderBackend::fail(
    VideoEncoderIssue issue_value) {
    state_ = State::Failed;
    return std::unexpected{std::move(issue_value)};
}

void FfmpegH264EncoderBackend::close() noexcept {
    encoder_.close();
    encoder_frame_.reset();
    converter_.reset();
    metadata_by_pts_.clear();
    output_ = {};
    maximum_delayed_frames_ = 0;
    last_submitted_ticks_.reset();
    last_emitted_ticks_.reset();
    state_ = State::Closed;
}

}  // namespace semilive::publisher::infra::ffmpeg
