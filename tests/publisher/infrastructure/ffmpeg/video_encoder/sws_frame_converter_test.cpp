#include "publisher/infrastructure/ffmpeg/video_encoder/sws_frame_converter.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using semilive::publisher::contracts::encoder::VideoEncoderOperation;
using semilive::publisher::infra::ffmpeg::AvFramePtr;
using semilive::publisher::infra::ffmpeg::SwsFrameConverter;
using semilive::publisher::model::BgraFrameBuffer;
using semilive::publisher::model::VideoPlacement;

struct BgraColor {
    std::uint8_t blue;
    std::uint8_t green;
    std::uint8_t red;
    std::uint8_t alpha = 255;
};

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

void require_near(const int actual,
                  const int expected,
                  const int tolerance,
                  const std::string_view message) {
    require(std::abs(actual - expected) <= tolerance, message);
}

BgraFrameBuffer solid_frame(const std::uint32_t width,
                            const std::uint32_t height,
                            const BgraColor color,
                            const std::uint32_t row_padding = 0) {
    BgraFrameBuffer frame;
    frame.width = width;
    frame.height = height;
    frame.stride = width * 4U + row_padding;
    frame.bgra.resize(static_cast<std::size_t>(frame.stride) * height);

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto offset = static_cast<std::size_t>(y) * frame.stride + x * 4U;
            frame.bgra[offset] = static_cast<std::byte>(color.blue);
            frame.bgra[offset + 1U] = static_cast<std::byte>(color.green);
            frame.bgra[offset + 2U] = static_cast<std::byte>(color.red);
            frame.bgra[offset + 3U] = static_cast<std::byte>(color.alpha);
        }
    }
    return frame;
}

int sample(const AVFrame& frame,
           const int plane,
           const std::uint32_t x,
           const std::uint32_t y) {
    const auto line_size = static_cast<std::size_t>(frame.linesize[plane]);
    return frame.data[plane][static_cast<std::size_t>(y) * line_size + x];
}

AvFramePtr output_frame(const std::uint32_t width,
                        const std::uint32_t height) {
    AvFramePtr frame{av_frame_alloc()};
    require(frame != nullptr, "the output AVFrame must be allocated");
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = static_cast<int>(width);
    frame->height = static_cast<int>(height);
    require(av_frame_get_buffer(frame.get(), 32) >= 0,
            "the output AVFrame buffer must be allocated");
    return frame;
}

void convert(SwsFrameConverter& converter,
             const BgraFrameBuffer& input,
             const VideoPlacement placement,
             AVFrame& output) {
    const auto result = converter.convert(input, placement, output);
    require(result.has_value(), "frame conversion must succeed");
}

void configuration_validates_and_describes_the_output_frame() {
    SwsFrameConverter converter;
    require(!converter.configure({0, 2}).has_value(),
            "zero output dimensions must be rejected");
    require(!converter.configure({3, 2}).has_value(),
            "odd YUV420P output dimensions must be rejected");
    require(converter.configure({2, 2}).has_value(),
            "valid output dimensions must be accepted");

    const auto input = solid_frame(2, 2, {0, 0, 255});
    auto frame = output_frame(2, 2);
    convert(converter, input, {0, 0, 2, 2}, *frame);
    require(frame->format == AV_PIX_FMT_YUV420P,
            "the output pixel format must be YUV420P");
    require(frame->width == 2 && frame->height == 2,
            "the output frame dimensions must match the configuration");
    require(frame->colorspace == AVCOL_SPC_BT709,
            "the output must declare the BT.709 color matrix");
    require(frame->color_range == AVCOL_RANGE_MPEG,
            "the output must declare limited range");
    require(frame->sample_aspect_ratio.num == 1 &&
                frame->sample_aspect_ratio.den == 1,
            "the output sample aspect ratio must be square");
}

void solid_red_is_converted_with_bt709_limited_range() {
    SwsFrameConverter converter;
    require(converter.configure({2, 2}).has_value(),
            "converter configuration must succeed");
    const auto input = solid_frame(2, 2, {0, 0, 255, 0});
    auto frame = output_frame(2, 2);
    convert(converter, input, {0, 0, 2, 2}, *frame);

    for (std::uint32_t y = 0; y < 2; ++y) {
        for (std::uint32_t x = 0; x < 2; ++x) {
            require_near(sample(*frame, 0, x, y), 63, 2,
                         "red luma must use BT.709 limited-range coefficients");
        }
    }
    require_near(sample(*frame, 1, 0, 0), 102, 2,
                 "red chroma U must use BT.709 limited-range coefficients");
    require_near(sample(*frame, 2, 0, 0), 240, 2,
                 "red chroma V must use BT.709 limited-range coefficients");
}

void placement_area_is_written_and_the_remaining_canvas_stays_black() {
    SwsFrameConverter converter;
    require(converter.configure({8, 4}).has_value(),
            "converter configuration must succeed");
    const auto input = solid_frame(2, 4, {0, 0, 255});
    auto frame = output_frame(8, 4);
    convert(converter, input, {2, 0, 2, 4}, *frame);

    for (std::uint32_t y = 0; y < 4; ++y) {
        require(sample(*frame, 0, 0, y) == 16 &&
                    sample(*frame, 0, 1, y) == 16,
                "the leading luma border must be limited-range black");
        require_near(sample(*frame, 0, 2, y), 63, 2,
                     "the placement luma must contain converted pixels");
        for (std::uint32_t x = 4; x < 8; ++x) {
            require(sample(*frame, 0, x, y) == 16,
                    "the trailing luma border must remain black");
        }
    }
    require(sample(*frame, 1, 0, 0) == 128 &&
                sample(*frame, 2, 0, 0) == 128,
            "the leading chroma border must be neutral black");
    require_near(sample(*frame, 1, 1, 0), 102, 2,
                 "the placement chroma U must contain converted pixels");
    require_near(sample(*frame, 2, 1, 0), 240, 2,
                 "the placement chroma V must contain converted pixels");
    require(sample(*frame, 1, 3, 0) == 128 &&
                sample(*frame, 2, 3, 0) == 128,
            "the trailing chroma border must remain neutral black");
}

void padded_input_and_input_size_changes_are_supported() {
    SwsFrameConverter converter;
    require(converter.configure({8, 4}).has_value(),
            "converter configuration must succeed");
    auto output = output_frame(8, 4);
    const auto portrait = solid_frame(2, 4, {255, 0, 0}, 8);
    convert(converter, portrait, {2, 0, 2, 4}, *output);
    AVFrame* const frame_address = output.get();
    std::uint8_t* const buffer_address = output->data[0];

    const auto landscape = solid_frame(4, 2, {0, 255, 0}, 4);
    convert(converter, landscape, {0, 0, 8, 4}, *output);
    require(output.get() == frame_address,
            "input size changes must reuse the output AVFrame object");
    require(output->data[0] == buffer_address,
            "a writable output buffer should be reused between conversions");
    require_near(sample(*output, 0, 0, 0), 173, 2,
                 "conversion after an input size change must use the new pixels");
}

void retained_frame_references_are_not_overwritten() {
    SwsFrameConverter converter;
    require(converter.configure({2, 2}).has_value(),
            "converter configuration must succeed");
    auto output = output_frame(2, 2);
    const auto red = solid_frame(2, 2, {0, 0, 255});
    convert(converter, red, {0, 0, 2, 2}, *output);
    AvFramePtr retained{av_frame_clone(output.get())};
    require(retained != nullptr,
            "the first output frame must be referenceable");

    const auto blue = solid_frame(2, 2, {255, 0, 0});
    convert(converter, blue, {0, 0, 2, 2}, *output);
    require_near(sample(*retained, 0, 0, 0), 63, 2,
                 "a retained frame reference must preserve its old pixels");
    require_near(sample(*output, 0, 0, 0), 32, 2,
                 "the writable output frame must contain the new pixels");
    converter.reset();
    require_near(sample(*output, 0, 0, 0), 32, 2,
                 "resetting the converter must not release the caller's frame");
}

void invalid_inputs_and_placements_are_rejected() {
    SwsFrameConverter converter;
    const auto valid = solid_frame(2, 2, {0, 0, 0});
    auto output = output_frame(4, 4);
    const auto before_configuration =
        converter.convert(valid, {0, 0, 2, 2}, *output);
    require(!before_configuration.has_value(),
            "conversion before configuration must fail");
    require(converter.configure({4, 4}).has_value(),
            "converter configuration must succeed");

    auto wrong_output = output_frame(2, 2);
    require(!converter.convert(
                valid, {0, 0, 4, 4}, *wrong_output).has_value(),
            "the caller-provided output must match the configured layout");

    auto short_stride = valid;
    short_stride.stride = 7;
    const auto invalid_input =
        converter.convert(short_stride, {0, 0, 4, 4}, *output);
    require(!invalid_input.has_value() &&
                invalid_input.error().operation == VideoEncoderOperation::ValidateInput,
            "an invalid BGRA layout must report input validation failure");

    auto undersized_buffer = valid;
    undersized_buffer.bgra.pop_back();
    require(!converter.convert(
                undersized_buffer, {0, 0, 4, 4}, *output).has_value(),
            "a BGRA buffer shorter than its declared layout must be rejected");

    const auto invalid_placement =
        converter.convert(valid, {1, 0, 2, 2}, *output);
    require(!invalid_placement.has_value() &&
                invalid_placement.error().operation ==
                    VideoEncoderOperation::CalculatePlacement,
            "an odd placement origin must report placement failure");

    converter.reset();
    require(!converter.convert(valid, {0, 0, 2, 2}, *output).has_value(),
            "reset must return the converter to its unconfigured state");
}

}  // namespace

int main() {
    try {
        configuration_validates_and_describes_the_output_frame();
        solid_red_is_converted_with_bt709_limited_range();
        placement_area_is_written_and_the_remaining_canvas_stays_black();
        padded_input_and_input_size_changes_are_supported();
        retained_frame_references_are_not_overwritten();
        invalid_inputs_and_placements_are_rejected();
    } catch (const std::exception& error) {
        std::cerr << "publisher sws frame converter tests failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher sws frame converter tests passed\n";
    return EXIT_SUCCESS;
}
