#pragma once

#include "publisher/model/video/captured_video_frame.hpp"
#include "publisher/model/video/encoded_video_access_unit.hpp"
#include "publisher/model/video/frame_rate.hpp"
#include "publisher/model/video/video_dimensions.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace semilive::publisher::contracts::encoder {

struct VideoEncoderConfig {
    model::VideoDimensions output{1920, 1080};
    model::FrameRate frame_rate{30, 1};
    std::uint64_t target_bit_rate = 4'000'000;
    std::uint32_t gop_size = 60;
};

struct VideoEncoderInfo {
    model::VideoDimensions output;
    model::FrameRate frame_rate;
    std::uint64_t target_bit_rate = 0;
    std::uint32_t gop_size = 0;
    std::uint32_t maximum_delayed_frames = 0;
    std::string encoder_name;
};

struct VideoEncodeBatch {
    std::vector<model::EncodedVideoAccessUnit> access_units;
    std::chrono::nanoseconds preprocessing_time{};
    std::chrono::nanoseconds codec_time{};
};

enum class VideoEncoderOperation : std::uint8_t {
    Open,
    ValidateInput,
    CalculatePlacement,
    ConvertFrame,
    SendFrame,
    ReceivePacket,
    Flush,
    Close,
};

struct VideoEncoderIssue {
    VideoEncoderOperation operation = VideoEncoderOperation::Open;
    std::int64_t native_code = 0;
    std::string message;
};

using VideoEncoderOpenResult =
    std::expected<VideoEncoderInfo, VideoEncoderIssue>;
using VideoEncodeResult =
    std::expected<VideoEncodeBatch, VideoEncoderIssue>;

class VideoEncoderBackend {
public:
    virtual ~VideoEncoderBackend() = default;

    VideoEncoderBackend(const VideoEncoderBackend&) = delete;
    VideoEncoderBackend& operator=(const VideoEncoderBackend&) = delete;
    VideoEncoderBackend(VideoEncoderBackend&&) = delete;
    VideoEncoderBackend& operator=(VideoEncoderBackend&&) = delete;

    [[nodiscard]] virtual VideoEncoderOpenResult open(
        const VideoEncoderConfig& config) = 0;
    [[nodiscard]] virtual VideoEncodeResult encode(
        const model::CapturedVideoFrame& frame) = 0;
    [[nodiscard]] virtual VideoEncodeResult flush() = 0;
    virtual void close() noexcept = 0;

protected:
    VideoEncoderBackend() = default;
};

}  // namespace semilive::publisher::contracts::encoder
