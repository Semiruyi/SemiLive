#pragma once

#include "publisher/contracts/encoder/video_encoder_backend.hpp"
#include "publisher/infrastructure/ffmpeg/ffmpeg_raii.hpp"
#include "publisher/model/video/bgra_frame_buffer.hpp"
#include "publisher/model/video/video_dimensions.hpp"
#include "publisher/model/video/video_placement.hpp"

#include <expected>

struct AVFrame;
namespace semilive::publisher::infra::ffmpeg {

class SwsFrameConverter {
public:
    SwsFrameConverter() = default;
    ~SwsFrameConverter();

    SwsFrameConverter(const SwsFrameConverter&) = delete;
    SwsFrameConverter& operator=(const SwsFrameConverter&) = delete;
    SwsFrameConverter(SwsFrameConverter&&) = delete;
    SwsFrameConverter& operator=(SwsFrameConverter&&) = delete;

    [[nodiscard]] std::expected<void, contracts::encoder::VideoEncoderIssue>
    configure(model::VideoDimensions output);

    [[nodiscard]] std::expected<void, contracts::encoder::VideoEncoderIssue>
    convert(const model::BgraFrameBuffer& input,
            const model::VideoPlacement& placement,
            AVFrame& output);

    void reset() noexcept;

private:
    [[nodiscard]] std::expected<void, contracts::encoder::VideoEncoderIssue>
    ensure_scaler(const model::BgraFrameBuffer& input,
                  const model::VideoPlacement& placement);

    [[nodiscard]] std::expected<void, contracts::encoder::VideoEncoderIssue>
    convert_directly(const model::BgraFrameBuffer& input, AVFrame& output);

    [[nodiscard]] std::expected<void, contracts::encoder::VideoEncoderIssue>
    convert_with_placement(const model::BgraFrameBuffer& input,
                           const model::VideoPlacement& placement,
                           AVFrame& output);

    AvFramePtr scaled_frame_;
    SwsContextPtr scaler_;
    model::VideoDimensions output_{};
    model::VideoDimensions scaler_input_{};
    model::VideoDimensions scaler_output_{};
};

}  // namespace semilive::publisher::infra::ffmpeg
