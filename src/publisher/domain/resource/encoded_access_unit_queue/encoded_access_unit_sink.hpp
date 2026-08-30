#pragma once

#include "publisher/domain/media/encoded_access_unit.hpp"

#include <cstdint>

namespace semilive::publisher::domain {

enum class EncodedAccessUnitPushResult : std::uint8_t {
    Accepted,
    Full,
};

class EncodedAccessUnitSink {
public:
    virtual ~EncodedAccessUnitSink() = default;

    EncodedAccessUnitSink(const EncodedAccessUnitSink&) = delete;
    EncodedAccessUnitSink& operator=(const EncodedAccessUnitSink&) = delete;
    EncodedAccessUnitSink(EncodedAccessUnitSink&&) = delete;
    EncodedAccessUnitSink& operator=(EncodedAccessUnitSink&&) = delete;

    [[nodiscard]] virtual EncodedAccessUnitPushResult try_push(EncodedAccessUnit&& access_unit) = 0;
    [[nodiscard]] virtual bool full() const noexcept = 0;

protected:
    EncodedAccessUnitSink() = default;
};

}  // namespace semilive::publisher::domain
