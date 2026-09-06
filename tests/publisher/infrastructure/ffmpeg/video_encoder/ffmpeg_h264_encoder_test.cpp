#include "publisher/infrastructure/ffmpeg/video_encoder/ffmpeg_h264_encoder.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace encoder = semilive::publisher::contracts::encoder;
namespace ffmpeg = semilive::publisher::infra::ffmpeg;
using ffmpeg::AvFramePtr;
using ffmpeg::FfmpegEncodedPacket;
using ffmpeg::FfmpegH264Encoder;

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

AvFramePtr allocate_frame(const encoder::VideoEncoderConfig& config) {
    AvFramePtr frame{av_frame_alloc()};
    require(frame != nullptr, "the YUV420P input frame must be allocated");
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = static_cast<int>(config.output.width);
    frame->height = static_cast<int>(config.output.height);
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->sample_aspect_ratio = AVRational{1, 1};
    require(av_frame_get_buffer(frame.get(), 32) >= 0,
            "the YUV420P input buffer must be allocated");
    return frame;
}

void fill_frame(AVFrame& frame,
                const std::int64_t pts,
                const std::uint8_t luma) {
    require(av_frame_make_writable(&frame) >= 0,
            "the YUV420P input frame must become writable");
    for (int row = 0; row < frame.height; ++row) {
        std::memset(frame.data[0] +
                        static_cast<std::size_t>(row) *
                            static_cast<std::size_t>(frame.linesize[0]),
                    luma, static_cast<std::size_t>(frame.width));
    }
    for (int row = 0; row < frame.height / 2; ++row) {
        std::memset(frame.data[1] +
                        static_cast<std::size_t>(row) *
                            static_cast<std::size_t>(frame.linesize[1]),
                    128, static_cast<std::size_t>(frame.width / 2));
        std::memset(frame.data[2] +
                        static_cast<std::size_t>(row) *
                            static_cast<std::size_t>(frame.linesize[2]),
                    128, static_cast<std::size_t>(frame.width / 2));
    }
    frame.pts = pts;
    frame.duration = 0;
}

void append(std::vector<FfmpegEncodedPacket>& destination,
            ffmpeg::FfmpegEncodeResult result,
            const std::string_view context) {
    require(result.has_value(),
            result ? context : result.error().message);
    destination.insert(
        destination.end(),
        std::make_move_iterator(result->begin()),
        std::make_move_iterator(result->end()));
}

std::uint8_t byte_value(const std::byte value) {
    return std::to_integer<std::uint8_t>(value);
}

bool contains_nal_type(const std::vector<std::byte>& annex_b,
                       const std::uint8_t expected_type) {
    for (std::size_t index = 0; index + 3U < annex_b.size(); ++index) {
        const bool three_byte_start =
            byte_value(annex_b[index]) == 0 &&
            byte_value(annex_b[index + 1U]) == 0 &&
            byte_value(annex_b[index + 2U]) == 1;
        const bool four_byte_start =
            index + 4U < annex_b.size() &&
            byte_value(annex_b[index]) == 0 &&
            byte_value(annex_b[index + 1U]) == 0 &&
            byte_value(annex_b[index + 2U]) == 0 &&
            byte_value(annex_b[index + 3U]) == 1;
        if (!three_byte_start && !four_byte_start) {
            continue;
        }

        const std::size_t nal_offset =
            index + (four_byte_start ? 4U : 3U);
        if (nal_offset < annex_b.size() &&
            (byte_value(annex_b[nal_offset]) & 0x1FU) == expected_type) {
            return true;
        }
    }
    return false;
}

void configuration_and_lifecycle_are_enforced() {
    FfmpegH264Encoder encoder;
    const auto config = test_config();
    auto frame = allocate_frame(config);
    fill_frame(*frame, 0, 32);

    const auto before_open = encoder.encode(*frame);
    require(!before_open &&
                before_open.error().operation ==
                    encoder::VideoEncoderOperation::State,
            "encoding before open must report a state issue");

    auto invalid = config;
    invalid.output.width = 63;
    const auto invalid_open = encoder.open(invalid);
    require(!invalid_open &&
                invalid_open.error().operation ==
                    encoder::VideoEncoderOperation::Open,
            "odd H.264 output dimensions must be rejected");

    const auto opened = encoder.open(config);
    require(opened.has_value(),
            opened ? "libx264 must open" : opened.error().message);
    require(opened->output == config.output &&
                opened->frame_rate == config.frame_rate &&
                opened->target_bit_rate == config.target_bit_rate &&
                opened->gop_size == config.gop_size,
            "open must report the effective H.264 configuration");
    require(opened->maximum_delayed_frames == 1,
            "the first backend must declare one delayed frame at most");
    require(opened->encoder_name == "libx264",
            "the encoder must not silently fall back from libx264");

    const auto second_open = encoder.open(config);
    require(!second_open &&
                second_open.error().operation ==
                    encoder::VideoEncoderOperation::State,
            "opening an active codec session must report a state issue");

    encoder.close();
    encoder.close();
    require(encoder.open(config).has_value(),
            "close must make the codec reusable");
    encoder.close();
}

void packets_are_annex_b_and_preserve_gapped_pts() {
    FfmpegH264Encoder encoder;
    const auto config = test_config();
    const auto opened = encoder.open(config);
    require(opened.has_value(),
            opened ? "libx264 must open" : opened.error().message);
    auto frame = allocate_frame(config);

    constexpr std::array<std::int64_t, 5> input_pts{
        0, 3'000, 9'000, 12'000, 15'000};
    std::vector<FfmpegEncodedPacket> packets;
    for (std::size_t index = 0; index < input_pts.size(); ++index) {
        fill_frame(*frame, input_pts[index],
                   static_cast<std::uint8_t>(32U + index * 16U));
        append(packets, encoder.encode(*frame),
               "encoding a valid YUV420P frame must succeed");
    }
    append(packets, encoder.flush(),
           "flushing accepted H.264 frames must succeed");

    require(packets.size() == input_pts.size(),
            "libx264 must emit one complete packet for each input frame");
    for (std::size_t index = 0; index < packets.size(); ++index) {
        require(!packets[index].annex_b.empty(),
                "every encoded packet must own non-empty bytes");
        require(packets[index].pts == input_pts[index],
                "packet PTS must preserve input gaps and ordering");
    }
    require(packets.front().key_frame,
            "the first H.264 packet must be a key frame");
    require(contains_nal_type(packets.front().annex_b, 7),
            "the first Annex-B access unit must contain SPS");
    require(contains_nal_type(packets.front().annex_b, 8),
            "the first Annex-B access unit must contain PPS");
    require(contains_nal_type(packets.front().annex_b, 5),
            "the first Annex-B access unit must contain IDR video data");

    std::size_t previous_key_frame = 0;
    for (std::size_t index = 1; index < packets.size(); ++index) {
        if (!packets[index].key_frame) {
            continue;
        }
        require(index - previous_key_frame <= config.gop_size,
                "key-frame distance must not exceed the configured GOP");
        require(contains_nal_type(packets[index].annex_b, 7) &&
                    contains_nal_type(packets[index].annex_b, 8),
                "every key access unit must repeat SPS and PPS");
        previous_key_frame = index;
    }
    require(packets.size() - 1U - previous_key_frame < config.gop_size,
            "the final GOP segment must stay within the configured size");

    const auto second_flush = encoder.flush();
    require(!second_flush &&
                second_flush.error().operation ==
                    encoder::VideoEncoderOperation::State,
            "a codec session may only be flushed once");
    const auto after_flush = encoder.encode(*frame);
    require(!after_flush &&
                after_flush.error().operation ==
                    encoder::VideoEncoderOperation::State,
            "encoding after flush must report a state issue");
    encoder.close();
}

void invalid_input_fails_the_session_until_close() {
    FfmpegH264Encoder encoder;
    const auto config = test_config();
    require(encoder.open(config).has_value(), "libx264 must open");
    auto frame = allocate_frame(config);

    fill_frame(*frame, 0, 48);
    require(encoder.encode(*frame).has_value(),
            "the first frame must be accepted");
    fill_frame(*frame, 0, 64);
    const auto duplicate = encoder.encode(*frame);
    require(!duplicate &&
                duplicate.error().operation ==
                    encoder::VideoEncoderOperation::ValidateInput,
            "duplicate input PTS must be rejected");

    fill_frame(*frame, 3'000, 80);
    const auto after_failure = encoder.encode(*frame);
    require(!after_failure &&
                after_failure.error().operation ==
                    encoder::VideoEncoderOperation::State,
            "a failed codec session must require close");

    encoder.close();
    require(encoder.open(config).has_value(),
            "close must reset a failed codec session");
    const auto empty_flush = encoder.flush();
    require(empty_flush.has_value() && empty_flush->empty(),
            "flushing a session without input must succeed without output");
    encoder.close();
}

}  // namespace

int main() {
    try {
        configuration_and_lifecycle_are_enforced();
        packets_are_annex_b_and_preserve_gapped_pts();
        invalid_input_fails_the_session_until_close();
    } catch (const std::exception& error) {
        std::cerr << "publisher FFmpeg H.264 encoder tests failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher FFmpeg H.264 encoder tests passed\n";
    return EXIT_SUCCESS;
}
