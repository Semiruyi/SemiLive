#include "publisher/infrastructure/ffmpeg/video_encoder/sws_frame_converter.hpp"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace semilive::publisher::infra::ffmpeg {
namespace {

using contracts::encoder::VideoEncoderIssue;
using contracts::encoder::VideoEncoderOperation;

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

std::optional<VideoEncoderIssue> validate_output(
    const model::VideoDimensions output) {
    if (output.width == 0 || output.height == 0 ||
        (output.width % 2U) != 0U || (output.height % 2U) != 0U) {
        return issue(VideoEncoderOperation::Open, 0,
                     "YUV420P output dimensions must be non-zero and even");
    }
    if (output.width > static_cast<std::uint32_t>(INT_MAX) ||
        output.height > static_cast<std::uint32_t>(INT_MAX)) {
        return issue(VideoEncoderOperation::Open, 0,
                     "output dimensions exceed FFmpeg integer limits");
    }
    return std::nullopt;
}

std::optional<VideoEncoderIssue> validate_input(
    const model::BgraFrameBuffer& input) {
    if (input.width == 0 || input.height == 0 || input.stride == 0) {
        return issue(VideoEncoderOperation::ValidateInput, 0,
                     "BGRA input dimensions and stride must be non-zero");
    }
    if (input.width > std::numeric_limits<std::uint32_t>::max() / 4U ||
        input.stride < input.width * 4U) {
        return issue(VideoEncoderOperation::ValidateInput, 0,
                     "BGRA input stride is smaller than one pixel row");
    }
    if (input.width > static_cast<std::uint32_t>(INT_MAX) ||
        input.height > static_cast<std::uint32_t>(INT_MAX) ||
        input.stride > static_cast<std::uint32_t>(INT_MAX)) {
        return issue(VideoEncoderOperation::ValidateInput, 0,
                     "BGRA input layout exceeds FFmpeg integer limits");
    }

    const auto height = static_cast<std::size_t>(input.height);
    const auto stride = static_cast<std::size_t>(input.stride);
    if (height > std::numeric_limits<std::size_t>::max() / stride ||
        input.bgra.size() < height * stride) {
        return issue(VideoEncoderOperation::ValidateInput, 0,
                     "BGRA input buffer does not cover its declared layout");
    }
    return std::nullopt;
}

std::optional<VideoEncoderIssue> validate_placement(
    const model::VideoPlacement& placement,
    const model::VideoDimensions output) {
    const bool invalid_extent =
        placement.width == 0 || placement.height == 0 ||
        (placement.x % 2U) != 0U || (placement.y % 2U) != 0U ||
        (placement.width % 2U) != 0U || (placement.height % 2U) != 0U;
    const bool outside_canvas =
        placement.x > output.width || placement.y > output.height ||
        placement.width > output.width - placement.x ||
        placement.height > output.height - placement.y;
    if (invalid_extent || outside_canvas) {
        return issue(VideoEncoderOperation::CalculatePlacement, 0,
                     "video placement must be an even rectangle inside the output");
    }
    return std::nullopt;
}

bool covers_output(const model::VideoPlacement& placement,
                   const model::VideoDimensions output) {
    return placement.x == 0 && placement.y == 0 &&
           placement.width == output.width &&
           placement.height == output.height;
}

std::optional<VideoEncoderIssue> validate_output_frame(
    const AVFrame& frame,
    const model::VideoDimensions expected) {
    if (frame.format != AV_PIX_FMT_YUV420P ||
        frame.width != static_cast<int>(expected.width) ||
        frame.height != static_cast<int>(expected.height)) {
        return issue(VideoEncoderOperation::ConvertFrame, 0,
                     "output frame must match the configured YUV420P layout");
    }
    if (frame.data[0] == nullptr || frame.data[1] == nullptr ||
        frame.data[2] == nullptr || frame.linesize[0] <= 0 ||
        frame.linesize[1] <= 0 || frame.linesize[2] <= 0 ||
        frame.buf[0] == nullptr) {
        return issue(VideoEncoderOperation::ConvertFrame, 0,
                     "output frame must have reference-counted image planes");
    }
    if (frame.linesize[0] < frame.width ||
        frame.linesize[1] < frame.width / 2 ||
        frame.linesize[2] < frame.width / 2) {
        return issue(VideoEncoderOperation::ConvertFrame, 0,
                     "output frame line sizes do not cover the image width");
    }
    return std::nullopt;
}

AvFramePtr allocate_yuv_frame(const model::VideoDimensions dimensions,
                              int& native_code) {
    AvFramePtr frame{av_frame_alloc()};
    if (!frame) {
        native_code = AVERROR(ENOMEM);
        return {};
    }

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = static_cast<int>(dimensions.width);
    frame->height = static_cast<int>(dimensions.height);
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->sample_aspect_ratio = AVRational{1, 1};
    native_code = av_frame_get_buffer(frame.get(), 32);
    if (native_code < 0) {
        return {};
    }
    return frame;
}

void describe_output_frame(AVFrame& frame) {
    frame.color_range = AVCOL_RANGE_MPEG;
    frame.colorspace = AVCOL_SPC_BT709;
    frame.color_primaries = AVCOL_PRI_BT709;
    frame.color_trc = AVCOL_TRC_BT709;
    frame.sample_aspect_ratio = AVRational{1, 1};
    frame.pts = AV_NOPTS_VALUE;
    frame.duration = 0;
}

void fill_plane(std::uint8_t* data,
                const int line_size,
                const std::uint32_t width,
                const std::uint32_t height,
                const std::uint8_t value) {
    const auto row_stride = static_cast<std::size_t>(line_size);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memset(data + static_cast<std::size_t>(row) * row_stride,
                    value, width);
    }
}

void fill_black(AVFrame& frame, const model::VideoDimensions output) {
    fill_plane(frame.data[0], frame.linesize[0], output.width, output.height, 16);
    fill_plane(frame.data[1], frame.linesize[1], output.width / 2U,
               output.height / 2U, 128);
    fill_plane(frame.data[2], frame.linesize[2], output.width / 2U,
               output.height / 2U, 128);
}

std::uint8_t* plane_offset(std::uint8_t* data,
                           const int line_size,
                           const std::uint32_t x,
                           const std::uint32_t y) {
    return data + static_cast<std::size_t>(y) *
                      static_cast<std::size_t>(line_size) + x;
}

void copy_plane(const std::uint8_t* source,
                const int source_line_size,
                std::uint8_t* destination,
                const int destination_line_size,
                const std::uint32_t width,
                const std::uint32_t height) {
    const auto source_stride = static_cast<std::size_t>(source_line_size);
    const auto destination_stride =
        static_cast<std::size_t>(destination_line_size);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(destination + static_cast<std::size_t>(row) * destination_stride,
                    source + static_cast<std::size_t>(row) * source_stride,
                    width);
    }
}

void place_scaled_frame(const AVFrame& scaled,
                        AVFrame& output,
                        const model::VideoPlacement& placement) {
    copy_plane(scaled.data[0], scaled.linesize[0],
               plane_offset(output.data[0], output.linesize[0],
                            placement.x, placement.y),
               output.linesize[0], placement.width, placement.height);
    copy_plane(scaled.data[1], scaled.linesize[1],
               plane_offset(output.data[1], output.linesize[1],
                            placement.x / 2U, placement.y / 2U),
               output.linesize[1], placement.width / 2U, placement.height / 2U);
    copy_plane(scaled.data[2], scaled.linesize[2],
               plane_offset(output.data[2], output.linesize[2],
                            placement.x / 2U, placement.y / 2U),
               output.linesize[2], placement.width / 2U, placement.height / 2U);
}

int scale_frame(SwsContext& scaler,
                const model::BgraFrameBuffer& input,
                AVFrame& output) {
    const std::uint8_t* source[] = {
        reinterpret_cast<const std::uint8_t*>(input.bgra.data()),
        nullptr, nullptr, nullptr};
    const int source_lines[] = {static_cast<int>(input.stride), 0, 0, 0};
    std::uint8_t* destination[] = {
        output.data[0], output.data[1], output.data[2], nullptr};
    const int destination_lines[] = {
        output.linesize[0], output.linesize[1], output.linesize[2], 0};
    return sws_scale(&scaler, source, source_lines, 0,
                     static_cast<int>(input.height), destination,
                      destination_lines);
}

std::expected<void, VideoEncoderIssue> make_writable(
    AVFrame& frame,
    const std::string_view context) {
    const int result = av_frame_make_writable(&frame);
    if (result < 0) {
        return std::unexpected{ffmpeg_issue(
            VideoEncoderOperation::ConvertFrame, result, context)};
    }
    return {};
}

std::expected<void, VideoEncoderIssue> scale_and_validate(
    SwsContext& scaler,
    const model::BgraFrameBuffer& input,
    AVFrame& output,
    const std::uint32_t expected_height) {
    const int scaled_height = scale_frame(scaler, input, output);
    if (scaled_height != static_cast<int>(expected_height)) {
        return std::unexpected{issue(
            VideoEncoderOperation::ConvertFrame, scaled_height,
            "libswscale did not produce the requested output height")};
    }
    return {};
}

std::expected<SwsContextPtr, VideoEncoderIssue> create_scaler(
    const model::VideoDimensions input,
    const model::VideoDimensions output) {
    SwsContextPtr scaler{sws_getContext(
        static_cast<int>(input.width), static_cast<int>(input.height),
        AV_PIX_FMT_BGRA, static_cast<int>(output.width),
        static_cast<int>(output.height), AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr)};
    if (!scaler) {
        return std::unexpected{ffmpeg_issue(
            VideoEncoderOperation::ConvertFrame, AVERROR(ENOMEM),
            "failed to create the BGRA-to-YUV420P scaling context")};
    }

    const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
    const int color_result = sws_setColorspaceDetails(
        scaler.get(), coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16);
    if (color_result < 0) {
        return std::unexpected{ffmpeg_issue(
            VideoEncoderOperation::ConvertFrame, color_result,
            "failed to configure BT.709 limited-range conversion")};
    }
    return scaler;
}

}  // namespace

SwsFrameConverter::~SwsFrameConverter() = default;

std::expected<void, VideoEncoderIssue> SwsFrameConverter::configure(
    const model::VideoDimensions output) {
    if (auto validation_error = validate_output(output)) {
        return std::unexpected{std::move(*validation_error)};
    }
    if (output_ == output) {
        return {};
    }

    reset();
    output_ = output;
    return {};
}

std::expected<void, VideoEncoderIssue> SwsFrameConverter::convert(
    const model::BgraFrameBuffer& input,
    const model::VideoPlacement& placement,
    AVFrame& output) {
    if (output_.width == 0 || output_.height == 0) {
        return std::unexpected{issue(
            VideoEncoderOperation::ConvertFrame, 0,
            "frame converter must be configured before conversion")};
    }
    if (auto validation_error = validate_input(input)) {
        return std::unexpected{std::move(*validation_error)};
    }
    if (auto validation_error = validate_placement(placement, output_)) {
        return std::unexpected{std::move(*validation_error)};
    }
    if (auto validation_error = validate_output_frame(output, output_)) {
        return std::unexpected{std::move(*validation_error)};
    }
    if (auto scaler_result = ensure_scaler(input, placement);
        !scaler_result.has_value()) {
        return std::unexpected{std::move(scaler_result.error())};
    }

    auto result = covers_output(placement, output_)
                      ? convert_directly(input, output)
                      : convert_with_placement(input, placement, output);
    if (!result.has_value()) {
        return result;
    }
    describe_output_frame(output);
    return {};
}

std::expected<void, VideoEncoderIssue> SwsFrameConverter::ensure_scaler(
    const model::BgraFrameBuffer& input,
    const model::VideoPlacement& placement) {
    const model::VideoDimensions input_size{input.width, input.height};
    const model::VideoDimensions scaled_size{placement.width, placement.height};
    const bool needs_intermediate_frame = !covers_output(placement, output_);
    if (scaler_ && scaler_input_ == input_size &&
        scaler_output_ == scaled_size &&
        (!needs_intermediate_frame || scaled_frame_)) {
        if (!needs_intermediate_frame) {
            scaled_frame_.reset();
        }
        return {};
    }

    int native_code = 0;
    AvFramePtr replacement_frame;
    if (needs_intermediate_frame) {
        replacement_frame = allocate_yuv_frame(scaled_size, native_code);
        if (!replacement_frame) {
            return std::unexpected{ffmpeg_issue(
                VideoEncoderOperation::ConvertFrame, native_code,
                "failed to allocate the scaled YUV420P frame")};
        }
    }
    auto replacement_scaler = create_scaler(input_size, scaled_size);
    if (!replacement_scaler.has_value()) {
        return std::unexpected{std::move(replacement_scaler.error())};
    }

    scaler_ = std::move(*replacement_scaler);
    scaled_frame_ = std::move(replacement_frame);
    scaler_input_ = input_size;
    scaler_output_ = scaled_size;
    return {};
}

std::expected<void, VideoEncoderIssue> SwsFrameConverter::convert_directly(
    const model::BgraFrameBuffer& input,
    AVFrame& output) {
    if (auto result = make_writable(
            output, "failed to make the output YUV420P frame writable");
        !result.has_value()) {
        return result;
    }
    return scale_and_validate(*scaler_, input, output, output_.height);
}

std::expected<void, VideoEncoderIssue> SwsFrameConverter::convert_with_placement(
    const model::BgraFrameBuffer& input,
    const model::VideoPlacement& placement,
    AVFrame& output) {
    if (auto result = make_writable(
            *scaled_frame_, "failed to make the scaled YUV420P frame writable");
        !result.has_value()) {
        return result;
    }
    if (auto result = scale_and_validate(
            *scaler_, input, *scaled_frame_, placement.height);
        !result.has_value()) {
        return result;
    }
    if (auto result = make_writable(
            output, "failed to make the output YUV420P frame writable");
        !result.has_value()) {
        return result;
    }

    fill_black(output, output_);
    place_scaled_frame(*scaled_frame_, output, placement);
    return {};
}

void SwsFrameConverter::reset() noexcept {
    scaler_.reset();
    scaled_frame_.reset();
    output_ = {};
    scaler_input_ = {};
    scaler_output_ = {};
}

}  // namespace semilive::publisher::infra::ffmpeg
