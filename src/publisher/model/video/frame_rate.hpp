#pragma once

#include <cstdint>

namespace semilive::publisher::model {

struct FrameRate {
    std::uint32_t numerator = 30;
    std::uint32_t denominator = 1;

    [[nodiscard]] friend constexpr bool operator==(
        const FrameRate&,
        const FrameRate&) = default;
};

}  // namespace semilive::publisher::model
