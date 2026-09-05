#pragma once

#include "publisher/model/video/video_dimensions.hpp"
#include "publisher/model/video/video_placement.hpp"

#include <cstdint>
#include <expected>

namespace semilive::publisher::domain {

enum class VideoPlacementError : std::uint8_t {
    EmptyInput,
    InvalidOutput,
    ScaledImageTooSmall,
};

[[nodiscard]] std::expected<model::VideoPlacement, VideoPlacementError>
calculate_video_placement(model::VideoDimensions input,
                          model::VideoDimensions output) noexcept;

}  // namespace semilive::publisher::domain
