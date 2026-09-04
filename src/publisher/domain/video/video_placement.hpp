#pragma once

#include <cstdint>
#include <expected>

namespace semilive::publisher::domain {

struct VideoDimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] friend constexpr bool operator==(
        const VideoDimensions&,
        const VideoDimensions&) = default;
};

struct VideoPlacement {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] friend constexpr bool operator==(
        const VideoPlacement&,
        const VideoPlacement&) = default;
};

enum class VideoPlacementError : std::uint8_t {
    EmptyInput,
    InvalidOutput,
    ScaledImageTooSmall,
};

[[nodiscard]] std::expected<VideoPlacement, VideoPlacementError>
calculate_video_placement(VideoDimensions input, VideoDimensions output) noexcept;

}  // namespace semilive::publisher::domain
