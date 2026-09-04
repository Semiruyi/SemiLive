#include "publisher/domain/video/video_placement.hpp"

#include <cstdint>

namespace semilive::publisher::domain {
namespace {

constexpr std::uint32_t floor_to_even(const std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(value & ~std::uint64_t{1});
}

constexpr std::uint32_t centered_even_offset(
    const std::uint32_t canvas_extent,
    const std::uint32_t image_extent) noexcept {
    // YUV420P requires an even origin. Rounding toward the leading edge can
    // leave that edge's border two pixels smaller than the opposite border.
    return ((canvas_extent - image_extent) / 2U) & ~std::uint32_t{1};
}

}  // namespace

std::expected<VideoPlacement, VideoPlacementError>
calculate_video_placement(const VideoDimensions input,
                          const VideoDimensions output) noexcept {
    if (input.width == 0 || input.height == 0) {
        return std::unexpected{VideoPlacementError::EmptyInput};
    }
    if (output.width == 0 || output.height == 0 ||
        (output.width % 2U) != 0U || (output.height % 2U) != 0U) {
        return std::unexpected{VideoPlacementError::InvalidOutput};
    }

    std::uint32_t scaled_width = output.width;
    std::uint32_t scaled_height = output.height;

    // Cross multiplication avoids floating-point rounding. Products of two
    // uint32_t values are representable by uint64_t, including boundary input.
    const auto input_cross =
        static_cast<std::uint64_t>(input.width) * output.height;
    const auto output_cross =
        static_cast<std::uint64_t>(output.width) * input.height;
    if (input_cross >= output_cross) {
        scaled_height = floor_to_even(
            static_cast<std::uint64_t>(output.width) * input.height / input.width);
    } else {
        scaled_width = floor_to_even(
            static_cast<std::uint64_t>(output.height) * input.width / input.height);
    }

    if (scaled_width == 0 || scaled_height == 0) {
        return std::unexpected{VideoPlacementError::ScaledImageTooSmall};
    }

    return VideoPlacement{
        centered_even_offset(output.width, scaled_width),
        centered_even_offset(output.height, scaled_height),
        scaled_width,
        scaled_height,
    };
}

}  // namespace semilive::publisher::domain
