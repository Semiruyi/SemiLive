#include "publisher/infrastructure/ffmpeg/video_encoder/ffmpeg_h264_encoder_backend.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace encoder = semilive::publisher::contracts::encoder;
namespace ffmpeg = semilive::publisher::infra::ffmpeg;
namespace model = semilive::publisher::model;

using ffmpeg::FfmpegH264EncoderBackend;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

encoder::VideoEncoderConfig test_config() {
    return encoder::VideoEncoderConfig{
        .output = {64, 64},
        .frame_rate = {30, 1},
        .target_bit_rate = 300'000,
        .gop_size = 2,
    };
}

model::CapturedVideoFrame captured_frame(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint64_t sequence,
    const model::MediaTime presentation_time) {
    auto image = std::make_shared<model::BgraFrameBuffer>();
    image->width = width;
    image->height = height;
    image->stride = width * 4U;
    image->bgra.resize(static_cast<std::size_t>(image->stride) * height);

    const auto blue = static_cast<std::byte>(32U + sequence * 17U);
    const auto green = static_cast<std::byte>(64U + sequence * 11U);
    const auto red = static_cast<std::byte>(96U + sequence * 7U);
    for (std::size_t offset = 0; offset < image->bgra.size(); offset += 4U) {
        image->bgra[offset] = blue;
        image->bgra[offset + 1U] = green;
        image->bgra[offset + 2U] = red;
        image->bgra[offset + 3U] = std::byte{255};
    }

    return model::CapturedVideoFrame{
        std::move(image),
        sequence,
        presentation_time,
        std::chrono::steady_clock::time_point{
            std::chrono::milliseconds{static_cast<std::int64_t>(sequence * 7U)}},
    };
}

void append(std::vector<model::EncodedVideoAccessUnit>& destination,
            encoder::VideoEncodeResult result,
            const std::string_view context) {
    require(result.has_value(), result ? context : result.error().message);
    require(result->preprocessing_time >= std::chrono::nanoseconds::zero(),
            "preprocessing duration must not be negative");
    require(result->codec_time >= std::chrono::nanoseconds::zero(),
            "codec duration must not be negative");
    destination.insert(destination.end(),
                       std::make_move_iterator(result->access_units.begin()),
                       std::make_move_iterator(result->access_units.end()));
}

std::uint8_t byte_value(const std::byte value) {
    return std::to_integer<std::uint8_t>(value);
}

bool contains_start_code(const std::vector<std::byte>& annex_b) {
    for (std::size_t index = 0; index + 3U < annex_b.size(); ++index) {
        const bool three_byte =
            byte_value(annex_b[index]) == 0 &&
            byte_value(annex_b[index + 1U]) == 0 &&
            byte_value(annex_b[index + 2U]) == 1;
        const bool four_byte =
            index + 4U < annex_b.size() &&
            byte_value(annex_b[index]) == 0 &&
            byte_value(annex_b[index + 1U]) == 0 &&
            byte_value(annex_b[index + 2U]) == 0 &&
            byte_value(annex_b[index + 3U]) == 1;
        if (three_byte || four_byte) {
            return true;
        }
    }
    return false;
}

void lifecycle_and_effective_configuration_are_enforced() {
    FfmpegH264EncoderBackend backend;
    const auto config = test_config();
    const auto frame = captured_frame(64, 64, 0, std::chrono::milliseconds{0});

    const auto before_open = backend.encode(frame);
    require(!before_open &&
                before_open.error().operation ==
                    encoder::VideoEncoderOperation::State,
            "encoding before open must report a state issue");

    const auto opened = backend.open(config);
    require(opened.has_value(),
            opened ? "libx264 backend must open" : opened.error().message);
    require(opened->output == config.output &&
                opened->frame_rate == config.frame_rate &&
                opened->target_bit_rate == config.target_bit_rate &&
                opened->gop_size == config.gop_size &&
                opened->maximum_delayed_frames == 1 &&
                opened->encoder_name == "libx264",
            "backend open must report the effective libx264 configuration");

    const auto second_open = backend.open(config);
    require(!second_open &&
                second_open.error().operation ==
                    encoder::VideoEncoderOperation::State,
            "opening an active backend must report a state issue");

    backend.close();
    backend.close();
    require(backend.open(config).has_value(),
            "close must make the backend reusable");
    const auto empty_flush = backend.flush();
    require(empty_flush.has_value() && empty_flush->access_units.empty() &&
                empty_flush->preprocessing_time ==
                    std::chrono::nanoseconds::zero(),
            "flushing an empty session must succeed without preprocessing");
    require(!backend.flush().has_value(),
            "a backend session may only be flushed once");
    require(!backend.encode(frame).has_value(),
            "encoding after flush must fail");
    backend.close();
}

void dimension_changes_and_gapped_pts_preserve_metadata() {
    FfmpegH264EncoderBackend backend;
    require(backend.open(test_config()).has_value(),
            "libx264 backend must open");

    const std::array frames{
        captured_frame(64, 64, 10, std::chrono::milliseconds{0}),
        captured_frame(32, 64, 14, std::chrono::milliseconds{50}),
        captured_frame(64, 32, 21, std::chrono::milliseconds{150}),
    };
    std::vector<model::EncodedVideoAccessUnit> access_units;
    for (const auto& frame : frames) {
        append(access_units, backend.encode(frame),
               "encoding a valid BGRA frame must succeed");
    }
    append(access_units, backend.flush(),
           "flushing accepted BGRA frames must succeed");

    require(access_units.size() == frames.size(),
            "backend must emit one access unit for every accepted input");
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto& access_unit = access_units[index];
        const auto& source = frames[index];
        require(!access_unit.annex_b.empty() &&
                    contains_start_code(access_unit.annex_b),
                "each backend output must own Annex-B H.264 bytes");
        require(access_unit.presentation_time == source.presentation_time &&
                    access_unit.source_sequence == source.sequence &&
                    access_unit.captured_at == source.captured_at,
                "access-unit metadata must match the input selected by packet PTS");
    }
    require(access_units.front().key_frame,
            "the first backend output must be a key frame");
    backend.close();
}

void invalid_inputs_fail_the_session_and_close_resets_it() {
    FfmpegH264EncoderBackend backend;
    const auto config = test_config();
    require(backend.open(config).has_value(),
            "libx264 backend must open");

    model::CapturedVideoFrame missing_image;
    const auto invalid = backend.encode(missing_image);
    require(!invalid &&
                invalid.error().operation ==
                    encoder::VideoEncoderOperation::ValidateInput,
            "a missing BGRA image must report input validation failure");
    const auto after_failure = backend.encode(
        captured_frame(64, 64, 0, std::chrono::milliseconds{0}));
    require(!after_failure &&
                after_failure.error().operation ==
                    encoder::VideoEncoderOperation::State,
            "a failed backend session must require close");

    backend.close();
    require(backend.open(config).has_value(),
            "close must reset a failed backend session");
    require(backend.encode(captured_frame(
                64, 64, 1, std::chrono::nanoseconds{0})).has_value(),
            "the first mapped PTS must be accepted");
    const auto duplicate_pts = backend.encode(captured_frame(
        64, 64, 2, std::chrono::nanoseconds{1}));
    require(!duplicate_pts &&
                duplicate_pts.error().operation ==
                    encoder::VideoEncoderOperation::ValidateInput,
            "different media times mapping to the same 90 kHz PTS must fail");

    backend.close();
    require(backend.open(config).has_value(),
            "backend must open after a mapped-PTS failure is closed");
    const auto too_narrow = backend.encode(captured_frame(
        1, 100, 3, std::chrono::milliseconds{0}));
    require(!too_narrow &&
                too_narrow.error().operation ==
                    encoder::VideoEncoderOperation::CalculatePlacement,
            "an input too narrow for even YUV420P placement must fail");
    backend.close();
}

}  // namespace

int main() {
    try {
        lifecycle_and_effective_configuration_are_enforced();
        dimension_changes_and_gapped_pts_preserve_metadata();
        invalid_inputs_fail_the_session_and_close_resets_it();
    } catch (const std::exception& error) {
        std::cerr << "publisher FFmpeg H.264 backend tests failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher FFmpeg H.264 backend tests passed\n";
    return EXIT_SUCCESS;
}
