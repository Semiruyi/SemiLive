#pragma once

#include <cstdint>

namespace semilive::publisher::model {

struct VideoPlacement {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] friend constexpr bool operator==(
        const VideoPlacement&,
        const VideoPlacement&) = default;
};

}  // namespace semilive::publisher::model
