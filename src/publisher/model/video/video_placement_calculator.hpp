#pragma once

#include "publisher/model/video/video_dimensions.hpp"
#include "publisher/model/video/video_placement.hpp"

#include <cstdint>
#include <expected>

namespace semilive::publisher::model {

enum class VideoPlacementError : std::uint8_t {
    EmptyInput,
    InvalidOutput,
    ScaledImageTooSmall,
};

[[nodiscard]] std::expected<VideoPlacement, VideoPlacementError>
calculate_video_placement(VideoDimensions input,
                          VideoDimensions output) noexcept;

}  // namespace semilive::publisher::model
