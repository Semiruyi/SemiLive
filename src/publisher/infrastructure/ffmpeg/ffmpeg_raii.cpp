#include "publisher/infrastructure/ffmpeg/ffmpeg_raii.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace semilive::publisher::infra::ffmpeg {

void AvFrameDeleter::operator()(AVFrame* frame) const noexcept {
    av_frame_free(&frame);
}

void SwsContextDeleter::operator()(SwsContext* context) const noexcept {
    sws_freeContext(context);
}

}  // namespace semilive::publisher::infra::ffmpeg
