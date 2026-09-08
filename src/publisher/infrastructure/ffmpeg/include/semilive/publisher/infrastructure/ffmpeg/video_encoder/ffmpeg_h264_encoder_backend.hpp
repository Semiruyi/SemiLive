#pragma once

#include <semilive/publisher/contracts/encoder/video_encoder_backend.hpp>

#include <memory>

namespace semilive::publisher::infra::ffmpeg {

class FfmpegH264EncoderBackend final
    : public contracts::encoder::VideoEncoderBackend {
public:
    FfmpegH264EncoderBackend();
    ~FfmpegH264EncoderBackend() override;

    FfmpegH264EncoderBackend(const FfmpegH264EncoderBackend&) = delete;
    FfmpegH264EncoderBackend& operator=(const FfmpegH264EncoderBackend&) = delete;
    FfmpegH264EncoderBackend(FfmpegH264EncoderBackend&&) = delete;
    FfmpegH264EncoderBackend& operator=(FfmpegH264EncoderBackend&&) = delete;

    [[nodiscard]] contracts::encoder::VideoEncoderOpenResult open(
        const contracts::encoder::VideoEncoderConfig& config) override;
    [[nodiscard]] contracts::encoder::VideoEncodeResult encode(
        const model::CapturedVideoFrame& frame) override;
    [[nodiscard]] contracts::encoder::VideoEncodeResult flush() override;
    void close() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::publisher::infra::ffmpeg
