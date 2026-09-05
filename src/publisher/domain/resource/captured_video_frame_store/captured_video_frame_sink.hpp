#pragma once

#include "publisher/model/video/captured_video_frame.hpp"

#include <cstdint>

namespace semilive::publisher::domain {

enum class CapturedVideoFramePushResult : std::uint8_t {
    Accepted,
    ReplacedOldest,
};

class CapturedVideoFrameSink {
public:
    virtual ~CapturedVideoFrameSink() = default;

    CapturedVideoFrameSink(const CapturedVideoFrameSink&) = delete;
    CapturedVideoFrameSink& operator=(const CapturedVideoFrameSink&) = delete;
    CapturedVideoFrameSink(CapturedVideoFrameSink&&) = delete;
    CapturedVideoFrameSink& operator=(CapturedVideoFrameSink&&) = delete;

    [[nodiscard]] virtual CapturedVideoFramePushResult try_push(
        model::CapturedVideoFrame&& frame) = 0;

protected:
    CapturedVideoFrameSink() = default;
};

}  // namespace semilive::publisher::domain
