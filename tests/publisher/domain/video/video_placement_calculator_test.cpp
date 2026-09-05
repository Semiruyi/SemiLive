#include "publisher/domain/video/video_placement_calculator.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using semilive::publisher::domain::VideoPlacementError;
using semilive::publisher::domain::calculate_video_placement;
using semilive::publisher::model::VideoDimensions;
using semilive::publisher::model::VideoPlacement;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

void require_placement(const VideoDimensions input,
                       const VideoDimensions output,
                       const VideoPlacement expected,
                       const std::string_view message) {
    const auto result = calculate_video_placement(input, output);
    require(result.has_value(), message);
    require(*result == expected, message);
}

void require_error(const VideoDimensions input,
                   const VideoDimensions output,
                   const VideoPlacementError expected,
                   const std::string_view message) {
    const auto result = calculate_video_placement(input, output);
    require(!result.has_value(), message);
    require(result.error() == expected, message);
}

void matching_aspect_ratio_fills_the_canvas() {
    require_placement(
        {1280, 720}, {1920, 1080}, {0, 0, 1920, 1080},
        "matching aspect ratio must fill the output canvas");
}

void narrower_input_adds_even_horizontal_borders() {
    require_placement(
        {4, 3}, {1920, 1080}, {240, 0, 1440, 1080},
        "4:3 input must be centered with horizontal borders");
}

void portrait_input_adds_even_horizontal_borders() {
    require_placement(
        {1080, 1920}, {1920, 1080}, {656, 0, 606, 1080},
        "portrait input must fit inside the landscape canvas");
}

void wider_input_adds_even_vertical_borders() {
    require_placement(
        {2560, 1080}, {1920, 1080}, {0, 134, 1920, 810},
        "ultrawide input must be centered with vertical borders");
}

void odd_scaled_dimensions_and_offsets_are_rounded_down() {
    require_placement(
        {1999, 2000}, {1920, 1080}, {420, 0, 1078, 1080},
        "scaled dimensions and placement origin must remain even");
}

void smallest_yuv420p_canvas_is_supported() {
    require_placement(
        {1, 1}, {2, 2}, {0, 0, 2, 2},
        "the smallest valid YUV420P canvas must be supported");
}

void uint32_boundaries_do_not_overflow_aspect_comparison() {
    constexpr auto maximum_even =
        std::numeric_limits<std::uint32_t>::max() - 1U;
    require_placement(
        {std::numeric_limits<std::uint32_t>::max(),
         std::numeric_limits<std::uint32_t>::max()},
        {maximum_even, maximum_even},
        {0, 0, maximum_even, maximum_even},
        "uint32 boundary dimensions must use overflow-safe arithmetic");
}

void invalid_dimensions_are_rejected() {
    require_error(
        {0, 1080}, {1920, 1080}, VideoPlacementError::EmptyInput,
        "zero input extent must be rejected");
    require_error(
        {1920, 1080}, {1919, 1080}, VideoPlacementError::InvalidOutput,
        "odd output extent must be rejected");
    require_error(
        {1920, 1080}, {0, 1080}, VideoPlacementError::InvalidOutput,
        "zero output extent must be rejected");
}

void unrepresentable_extreme_aspect_ratio_is_rejected() {
    require_error(
        {std::numeric_limits<std::uint32_t>::max(), 1}, {2, 2},
        VideoPlacementError::ScaledImageTooSmall,
        "an image dimension below two output pixels must be rejected");
}

}  // namespace

int main() {
    try {
        matching_aspect_ratio_fills_the_canvas();
        narrower_input_adds_even_horizontal_borders();
        portrait_input_adds_even_horizontal_borders();
        wider_input_adds_even_vertical_borders();
        odd_scaled_dimensions_and_offsets_are_rounded_down();
        smallest_yuv420p_canvas_is_supported();
        uint32_boundaries_do_not_overflow_aspect_comparison();
        invalid_dimensions_are_rejected();
        unrepresentable_extreme_aspect_ratio_is_rejected();
    } catch (const std::exception& error) {
        std::cerr << "publisher video placement calculator tests failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher video placement calculator tests passed\n";
    return EXIT_SUCCESS;
}
