#pragma once

#include <cstdint>

namespace semilive::publisher::model {

struct VideoDimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] friend constexpr bool operator==(
        const VideoDimensions&,
        const VideoDimensions&) = default;
};

}  // namespace semilive::publisher::model
