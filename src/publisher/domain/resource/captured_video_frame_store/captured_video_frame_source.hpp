#pragma once

#include "publisher/domain/media/captured_video_frame.hpp"

#include <optional>

namespace semilive::publisher::domain {

class CapturedVideoFrameSource {
public:
    virtual ~CapturedVideoFrameSource() = default;

    CapturedVideoFrameSource(const CapturedVideoFrameSource&) = delete;
    CapturedVideoFrameSource& operator=(const CapturedVideoFrameSource&) = delete;
    CapturedVideoFrameSource(CapturedVideoFrameSource&&) = delete;
    CapturedVideoFrameSource& operator=(CapturedVideoFrameSource&&) = delete;

    [[nodiscard]] virtual std::optional<CapturedVideoFrame> try_pop() = 0;
    [[nodiscard]] virtual bool empty() const noexcept = 0;

protected:
    CapturedVideoFrameSource() = default;
};

}  // namespace semilive::publisher::domain
