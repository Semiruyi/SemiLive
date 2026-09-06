#pragma once

#include "publisher/contracts/encoder/video_encoder_backend.hpp"
#include "publisher/infrastructure/ffmpeg/ffmpeg_raii.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <vector>

struct AVFrame;

namespace semilive::publisher::infra::ffmpeg {

struct FfmpegEncodedPacket {
    std::vector<std::byte> annex_b;
    std::int64_t pts = 0;
    bool key_frame = false;
};

using FfmpegEncodeResult =
    std::expected<std::vector<FfmpegEncodedPacket>,
                  contracts::encoder::VideoEncoderIssue>;

class FfmpegH264Encoder {
public:
    FfmpegH264Encoder() = default;
    ~FfmpegH264Encoder();

    FfmpegH264Encoder(const FfmpegH264Encoder&) = delete;
    FfmpegH264Encoder& operator=(const FfmpegH264Encoder&) = delete;
    FfmpegH264Encoder(FfmpegH264Encoder&&) = delete;
    FfmpegH264Encoder& operator=(FfmpegH264Encoder&&) = delete;

    [[nodiscard]] contracts::encoder::VideoEncoderOpenResult open(
        const contracts::encoder::VideoEncoderConfig& config);
    [[nodiscard]] FfmpegEncodeResult encode(AVFrame& frame);
    [[nodiscard]] FfmpegEncodeResult flush();
    void close() noexcept;

private:
    enum class State : std::uint8_t {
        Closed,
        Open,
        Flushed,
        Failed,
    };

    [[nodiscard]] FfmpegEncodeResult send_with_retry(
        const AVFrame* frame,
        contracts::encoder::VideoEncoderOperation operation,
        std::string_view error_context);
    [[nodiscard]] FfmpegEncodeResult receive_available();
    [[nodiscard]] FfmpegEncodeResult receive_until_eof();
    [[nodiscard]] FfmpegEncodeResult fail(
        contracts::encoder::VideoEncoderIssue issue);

    AvCodecContextPtr context_;
    AvPacketPtr receive_packet_;
    std::optional<std::int64_t> last_submitted_pts_;
    State state_ = State::Closed;
};

}  // namespace semilive::publisher::infra::ffmpeg
