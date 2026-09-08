#include "ffmpeg_raii.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace semilive::publisher::infra::ffmpeg {

void AvCodecContextDeleter::operator()(AVCodecContext* context) const noexcept {
    avcodec_free_context(&context);
}

void AvFrameDeleter::operator()(AVFrame* frame) const noexcept {
    av_frame_free(&frame);
}

void AvPacketDeleter::operator()(AVPacket* packet) const noexcept {
    av_packet_free(&packet);
}

void SwsContextDeleter::operator()(SwsContext* context) const noexcept {
    sws_freeContext(context);
}

}  // namespace semilive::publisher::infra::ffmpeg
