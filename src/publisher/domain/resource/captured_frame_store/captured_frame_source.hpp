#pragma once

#include "publisher/domain/media/captured_frame.hpp"

#include <optional>

namespace semilive::publisher::domain {

class CapturedFrameSource {
public:
    virtual ~CapturedFrameSource() = default;

    CapturedFrameSource(const CapturedFrameSource&) = delete;
    CapturedFrameSource& operator=(const CapturedFrameSource&) = delete;
    CapturedFrameSource(CapturedFrameSource&&) = delete;
    CapturedFrameSource& operator=(CapturedFrameSource&&) = delete;

    [[nodiscard]] virtual std::optional<CapturedFrame> try_pop() = 0;
    [[nodiscard]] virtual bool empty() const noexcept = 0;

protected:
    CapturedFrameSource() = default;
};

}  // namespace semilive::publisher::domain
