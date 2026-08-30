#pragma once

#include "publisher/domain/media/captured_frame.hpp"

#include <cstdint>

namespace semilive::publisher::domain {

enum class CapturedFramePushResult : std::uint8_t {
    Accepted,
    ReplacedOldest,
};

class CapturedFrameSink {
public:
    virtual ~CapturedFrameSink() = default;

    CapturedFrameSink(const CapturedFrameSink&) = delete;
    CapturedFrameSink& operator=(const CapturedFrameSink&) = delete;
    CapturedFrameSink(CapturedFrameSink&&) = delete;
    CapturedFrameSink& operator=(CapturedFrameSink&&) = delete;

    [[nodiscard]] virtual CapturedFramePushResult try_push(CapturedFrame&& frame) = 0;

protected:
    CapturedFrameSink() = default;
};

}  // namespace semilive::publisher::domain
