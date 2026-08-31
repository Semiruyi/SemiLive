#pragma once

#include "publisher/domain/media/encoded_video_access_unit.hpp"

#include <cstdint>

namespace semilive::publisher::domain {

enum class EncodedVideoAccessUnitPushResult : std::uint8_t {
    Accepted,
    Full,
};

class EncodedVideoAccessUnitSink {
public:
    virtual ~EncodedVideoAccessUnitSink() = default;

    EncodedVideoAccessUnitSink(const EncodedVideoAccessUnitSink&) = delete;
    EncodedVideoAccessUnitSink& operator=(const EncodedVideoAccessUnitSink&) = delete;
    EncodedVideoAccessUnitSink(EncodedVideoAccessUnitSink&&) = delete;
    EncodedVideoAccessUnitSink& operator=(EncodedVideoAccessUnitSink&&) = delete;

    [[nodiscard]] virtual EncodedVideoAccessUnitPushResult try_push(EncodedVideoAccessUnit&& access_unit) = 0;
    [[nodiscard]] virtual bool full() const noexcept = 0;

protected:
    EncodedVideoAccessUnitSink() = default;
};

}  // namespace semilive::publisher::domain
