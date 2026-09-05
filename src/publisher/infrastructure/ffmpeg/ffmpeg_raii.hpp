#pragma once

#include <memory>

struct AVFrame;
struct SwsContext;

namespace semilive::publisher::infra::ffmpeg {

struct AvFrameDeleter {
    void operator()(AVFrame* frame) const noexcept;
};

struct SwsContextDeleter {
    void operator()(SwsContext* context) const noexcept;
};

using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

}  // namespace semilive::publisher::infra::ffmpeg
