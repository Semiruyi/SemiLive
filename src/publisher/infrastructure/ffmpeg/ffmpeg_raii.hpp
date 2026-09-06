#pragma once

#include <memory>

struct AVFrame;
struct AVCodecContext;
struct AVPacket;
struct SwsContext;

namespace semilive::publisher::infra::ffmpeg {

struct AvCodecContextDeleter {
    void operator()(AVCodecContext* context) const noexcept;
};

struct AvFrameDeleter {
    void operator()(AVFrame* frame) const noexcept;
};

struct AvPacketDeleter {
    void operator()(AVPacket* packet) const noexcept;
};

struct SwsContextDeleter {
    void operator()(SwsContext* context) const noexcept;
};

using AvCodecContextPtr =
    std::unique_ptr<AVCodecContext, AvCodecContextDeleter>;
using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;
using AvPacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

}  // namespace semilive::publisher::infra::ffmpeg
