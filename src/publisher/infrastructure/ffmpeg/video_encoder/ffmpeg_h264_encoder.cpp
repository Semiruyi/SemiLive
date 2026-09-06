#include "publisher/infrastructure/ffmpeg/video_encoder/ffmpeg_h264_encoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace semilive::publisher::infra::ffmpeg {
namespace {

using contracts::encoder::VideoEncoderConfig;
using contracts::encoder::VideoEncoderInfo;
using contracts::encoder::VideoEncoderIssue;
using contracts::encoder::VideoEncoderOpenResult;
using contracts::encoder::VideoEncoderOperation;

constexpr int kEncoderClockRate = 90'000;
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

std::optional<VideoEncoderIssue> validate_config(
    const VideoEncoderConfig& config) {
    if (config.output.width == 0 || config.output.height == 0 ||
        (config.output.width % 2U) != 0U ||
        (config.output.height % 2U) != 0U) {
        return issue(VideoEncoderOperation::Open, 0,
                     "H.264 output dimensions must be non-zero and even");
    }
    if (config.output.width > static_cast<std::uint32_t>(INT_MAX) ||
        config.output.height > static_cast<std::uint32_t>(INT_MAX)) {
        return issue(VideoEncoderOperation::Open, 0,
                     "H.264 output dimensions exceed FFmpeg integer limits");
    }
    const int image_size_result =
        av_image_check_size(config.output.width, config.output.height, 0, nullptr);
    if (image_size_result < 0) {
        return ffmpeg_issue(VideoEncoderOperation::Open, image_size_result,
                            "FFmpeg rejected the H.264 output dimensions");
    }
    if (config.frame_rate.numerator == 0 ||
        config.frame_rate.denominator == 0) {
        return issue(VideoEncoderOperation::Open, 0,
                     "H.264 frame rate numerator and denominator must be positive");
    }
    if (config.frame_rate.numerator > static_cast<std::uint32_t>(INT_MAX) ||
        config.frame_rate.denominator > static_cast<std::uint32_t>(INT_MAX)) {
        return issue(VideoEncoderOperation::Open, 0,
                     "H.264 frame rate exceeds FFmpeg rational limits");
    }
    if (config.target_bit_rate == 0 ||
        config.target_bit_rate >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return issue(VideoEncoderOperation::Open, 0,
                     "H.264 target bit rate exceeds FFmpeg integer limits");
    }
    if (config.gop_size == 0 ||
        config.gop_size > static_cast<std::uint32_t>(INT_MAX)) {
        return issue(VideoEncoderOperation::Open, 0,
                     "H.264 GOP size exceeds FFmpeg integer limits");
    }
    return std::nullopt;
}

std::optional<VideoEncoderIssue> set_option(AVCodecContext& context,
                                            const char* name,
                                            const char* value) {
    if (context.priv_data == nullptr) {
        return issue(VideoEncoderOperation::Open, AVERROR(EINVAL),
                     "libx264 private encoder state is unavailable");
    }
    const int result = av_opt_set(context.priv_data, name, value, 0);
    if (result < 0) {
        return ffmpeg_issue(
            VideoEncoderOperation::Open, result,
            std::string{"failed to set libx264 option '"} + name + "'");
    }
    return std::nullopt;
}

std::optional<VideoEncoderIssue> validate_opened_context(
    const AVCodecContext& context,
    const VideoEncoderConfig& config) {
    const AVRational requested_frame_rate{
        static_cast<int>(config.frame_rate.numerator),
        static_cast<int>(config.frame_rate.denominator),
    };
    const bool invalid =
        context.width != static_cast<int>(config.output.width) ||
        context.height != static_cast<int>(config.output.height) ||
        context.pix_fmt != AV_PIX_FMT_YUV420P ||
        av_cmp_q(context.time_base, AVRational{1, kEncoderClockRate}) != 0 ||
        av_cmp_q(context.framerate, requested_frame_rate) != 0 ||
        context.bit_rate != static_cast<std::int64_t>(config.target_bit_rate) ||
        context.gop_size != static_cast<int>(config.gop_size) ||
        context.max_b_frames != 0 ||
        (context.flags & AV_CODEC_FLAG_GLOBAL_HEADER) != 0 ||
        context.color_range != AVCOL_RANGE_MPEG ||
        context.colorspace != AVCOL_SPC_BT709 ||
        context.color_primaries != AVCOL_PRI_BT709 ||
        context.color_trc != AVCOL_TRC_BT709 ||
        av_cmp_q(context.sample_aspect_ratio, AVRational{1, 1}) != 0;
    if (invalid) {
        return issue(VideoEncoderOperation::Open, 0,
                     "libx264 opened with parameters outside the H.264 baseline");
    }
    return std::nullopt;
}

std::optional<VideoEncoderIssue> validate_frame(
    const AVFrame& frame,
    const AVCodecContext& context,
    const std::optional<std::int64_t> last_submitted_pts) {
    if (frame.format != context.pix_fmt ||
        frame.width != context.width ||
        frame.height != context.height) {
        return issue(VideoEncoderOperation::ValidateInput, 0,
                     "encoder input must match the opened YUV420P layout");
    }
    if (frame.data[0] == nullptr || frame.data[1] == nullptr ||
        frame.data[2] == nullptr || frame.buf[0] == nullptr ||
        frame.linesize[0] < frame.width ||
        frame.linesize[1] < frame.width / 2 ||
        frame.linesize[2] < frame.width / 2) {
        return issue(VideoEncoderOperation::ValidateInput, 0,
                     "encoder input must contain reference-counted YUV420P planes");
    }
    if (frame.pts == AV_NOPTS_VALUE || frame.pts < 0 ||
        (last_submitted_pts && frame.pts <= *last_submitted_pts)) {
        return issue(VideoEncoderOperation::ValidateInput, 0,
                     "encoder input PTS must be non-negative and strictly increasing");
    }
    if (frame.color_range != AVCOL_RANGE_MPEG ||
        frame.colorspace != AVCOL_SPC_BT709 ||
        frame.color_primaries != AVCOL_PRI_BT709 ||
        frame.color_trc != AVCOL_TRC_BT709 ||
        av_cmp_q(frame.sample_aspect_ratio, AVRational{1, 1}) != 0) {
        return issue(VideoEncoderOperation::ValidateInput, 0,
                     "encoder input must declare BT.709 limited-range square pixels");
    }
    return std::nullopt;
}

struct ReceiveNeedsInput {};
struct ReceiveEndOfStream {};

using ReceiveEvent = std::variant<
    FfmpegEncodedPacket,
    ReceiveNeedsInput,
    ReceiveEndOfStream>;
using ReceiveOneResult =
    std::expected<ReceiveEvent, VideoEncoderIssue>;

ReceiveOneResult receive_one(AVCodecContext& context,
                             AVPacket& receive_packet) {
    av_packet_unref(&receive_packet);
    const int receive_result =
        avcodec_receive_packet(&context, &receive_packet);
    if (receive_result == AVERROR(EAGAIN)) {
        return ReceiveEvent{ReceiveNeedsInput{}};
    }
    if (receive_result == AVERROR_EOF) {
        return ReceiveEvent{ReceiveEndOfStream{}};
    }
    if (receive_result < 0) {
        return std::unexpected{ffmpeg_issue(
            VideoEncoderOperation::ReceivePacket, receive_result,
            "failed to receive an encoded H.264 packet")};
    }
    if (receive_packet.data == nullptr || receive_packet.size <= 0) {
        return std::unexpected{issue(
            VideoEncoderOperation::ReceivePacket, 0,
            "libx264 returned an empty H.264 packet")};
    }
    if (receive_packet.pts == AV_NOPTS_VALUE) {
        return std::unexpected{issue(
            VideoEncoderOperation::ReceivePacket, 0,
            "libx264 returned a packet without PTS")};
    }

    FfmpegEncodedPacket packet;
    const auto packet_size =
        static_cast<std::size_t>(receive_packet.size);
    packet.annex_b.resize(packet_size);
    std::memcpy(packet.annex_b.data(), receive_packet.data, packet_size); // TODO 此处可考虑优化为0拷贝
    packet.pts = receive_packet.pts;
    packet.key_frame = (receive_packet.flags & AV_PKT_FLAG_KEY) != 0;
    return ReceiveEvent{std::move(packet)};
}

void append_packets(std::vector<FfmpegEncodedPacket>& destination,
                    std::vector<FfmpegEncodedPacket> source) {
    destination.insert(destination.end(),
                       std::make_move_iterator(source.begin()),
                       std::make_move_iterator(source.end()));
}

}  // namespace

FfmpegH264Encoder::~FfmpegH264Encoder() {
    close();
}

VideoEncoderOpenResult FfmpegH264Encoder::open(
    const VideoEncoderConfig& config) {
    if (state_ != State::Closed) {
        return std::unexpected{issue(
            VideoEncoderOperation::State, 0,
            "FFmpeg H.264 encoder can only open from the closed state")};
    }
    if (auto validation_error = validate_config(config)) {
        return std::unexpected{std::move(*validation_error)};
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (codec == nullptr) {
        return std::unexpected{issue(
            VideoEncoderOperation::Open, AVERROR_ENCODER_NOT_FOUND,
            "required FFmpeg encoder 'libx264' is unavailable")};
    }

    AvCodecContextPtr replacement_context{avcodec_alloc_context3(codec)};
    if (!replacement_context) {
        return std::unexpected{ffmpeg_issue(
            VideoEncoderOperation::Open, AVERROR(ENOMEM),
            "failed to allocate the libx264 codec context")};
    }

    replacement_context->width = static_cast<int>(config.output.width);
    replacement_context->height = static_cast<int>(config.output.height);
    replacement_context->pix_fmt = AV_PIX_FMT_YUV420P;
    replacement_context->time_base = AVRational{1, kEncoderClockRate};
    replacement_context->framerate = AVRational{
        static_cast<int>(config.frame_rate.numerator),
        static_cast<int>(config.frame_rate.denominator),
    };
    replacement_context->bit_rate =
        static_cast<std::int64_t>(config.target_bit_rate);
    replacement_context->gop_size = static_cast<int>(config.gop_size);
    replacement_context->max_b_frames = 0;
    replacement_context->sample_aspect_ratio = AVRational{1, 1};
    replacement_context->color_range = AVCOL_RANGE_MPEG;
    replacement_context->colorspace = AVCOL_SPC_BT709;
    replacement_context->color_primaries = AVCOL_PRI_BT709;
    replacement_context->color_trc = AVCOL_TRC_BT709;
    replacement_context->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER;

    if (auto result = set_option(*replacement_context, "preset", "ultrafast")) {
        return std::unexpected{std::move(*result)};
    }
    if (auto result = set_option(*replacement_context, "tune", "zerolatency")) {
        return std::unexpected{std::move(*result)};
    }
    if (auto result = set_option(
            *replacement_context, "x264-params",
            "repeat-headers=1:annexb=1")) {
        return std::unexpected{std::move(*result)};
    }

    const int open_result =
        avcodec_open2(replacement_context.get(), codec, nullptr);
    if (open_result < 0) {
        return std::unexpected{ffmpeg_issue(
            VideoEncoderOperation::Open, open_result,
            "failed to open the required libx264 encoder")};
    }
    if (auto validation_error =
            validate_opened_context(*replacement_context, config)) {
        return std::unexpected{std::move(*validation_error)};
    }

    AvPacketPtr replacement_packet{av_packet_alloc()};
    if (!replacement_packet) {
        return std::unexpected{ffmpeg_issue(
            VideoEncoderOperation::Open, AVERROR(ENOMEM),
            "failed to allocate the H.264 receive packet")};
    }

    VideoEncoderInfo info;
    info.output = config.output;
    info.frame_rate = config.frame_rate;
    info.target_bit_rate =
        static_cast<std::uint64_t>(replacement_context->bit_rate);
    info.gop_size =
        static_cast<std::uint32_t>(replacement_context->gop_size);
    info.maximum_delayed_frames = kMaximumDelayedFrames;
    info.encoder_name = codec->name;

    context_ = std::move(replacement_context);
    receive_packet_ = std::move(replacement_packet);
    last_submitted_pts_.reset();
    state_ = State::Open;
    return info;
}

FfmpegEncodeResult FfmpegH264Encoder::encode(AVFrame& frame) {
    if (state_ != State::Open) {
        return std::unexpected{issue(
            VideoEncoderOperation::State, 0,
            "FFmpeg H.264 encoder must be open before encoding")};
    }
    if (auto validation_error =
            validate_frame(frame, *context_, last_submitted_pts_)) {
        return fail(std::move(*validation_error));
    }

    auto packets = send_with_retry(
        &frame, VideoEncoderOperation::SendFrame,
        "failed to send a YUV420P frame to libx264");
    if (!packets) {
        return fail(std::move(packets.error()));
    }
    auto received = receive_available();
    if (!received) {
        return fail(std::move(received.error()));
    }
    append_packets(*packets, std::move(*received));
    last_submitted_pts_ = frame.pts;
    return packets;
}

FfmpegEncodeResult FfmpegH264Encoder::flush() {
    if (state_ != State::Open) {
        return std::unexpected{issue(
            VideoEncoderOperation::State, 0,
            "FFmpeg H.264 encoder can only flush an open session once")};
    }

    auto packets = send_with_retry(
        nullptr, VideoEncoderOperation::Flush,
        "failed to start draining the libx264 encoder");
    if (!packets) {
        return fail(std::move(packets.error()));
    }
    auto drained = receive_until_eof();
    if (!drained) {
        return fail(std::move(drained.error()));
    }
    append_packets(*packets, std::move(*drained));
    state_ = State::Flushed;
    return packets;
}

FfmpegEncodeResult FfmpegH264Encoder::send_with_retry(
    const AVFrame* frame,
    const VideoEncoderOperation operation,
    const std::string_view error_context) {
    std::vector<FfmpegEncodedPacket> packets;
    int send_result = avcodec_send_frame(context_.get(), frame);
    if (send_result == AVERROR(EAGAIN)) {
        auto pending = receive_available();
        if (!pending) {
            return std::unexpected{std::move(pending.error())};
        }
        append_packets(packets, std::move(*pending));
        send_result = avcodec_send_frame(context_.get(), frame);
    }
    if (send_result < 0) {
        return std::unexpected{ffmpeg_issue(
            operation, send_result, error_context)};
    }
    return packets;
}

FfmpegEncodeResult FfmpegH264Encoder::receive_available() {
    std::vector<FfmpegEncodedPacket> packets;
    while (true) {
        auto event = receive_one(*context_, *receive_packet_);
        if (!event) {
            return std::unexpected{std::move(event.error())};
        }
        if (auto* packet = std::get_if<FfmpegEncodedPacket>(&*event)) {
            packets.push_back(std::move(*packet));
            continue;
        }
        if (std::holds_alternative<ReceiveNeedsInput>(*event)) {
            return packets;
        }
        return std::unexpected{issue(
            VideoEncoderOperation::ReceivePacket, AVERROR_EOF,
            "libx264 reached EOF before the encoder was flushed")};
    }
}

FfmpegEncodeResult FfmpegH264Encoder::receive_until_eof() {
    std::vector<FfmpegEncodedPacket> packets;
    while (true) {
        auto event = receive_one(*context_, *receive_packet_);
        if (!event) {
            return std::unexpected{std::move(event.error())};
        }
        if (auto* packet = std::get_if<FfmpegEncodedPacket>(&*event)) {
            packets.push_back(std::move(*packet));
            continue;
        }
        if (std::holds_alternative<ReceiveEndOfStream>(*event)) {
            return packets;
        }
        return std::unexpected{issue(
            VideoEncoderOperation::ReceivePacket, AVERROR(EAGAIN),
            "libx264 requested more input after flush was accepted")};
    }
}

FfmpegEncodeResult FfmpegH264Encoder::fail(VideoEncoderIssue issue) {
    state_ = State::Failed;
    return std::unexpected{std::move(issue)};
}

void FfmpegH264Encoder::close() noexcept {
    context_.reset();
    receive_packet_.reset();
    last_submitted_pts_.reset();
    state_ = State::Closed;
}

}  // namespace semilive::publisher::infra::ffmpeg
