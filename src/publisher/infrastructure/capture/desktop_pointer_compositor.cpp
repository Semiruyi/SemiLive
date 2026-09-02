#include "publisher/infrastructure/capture/desktop_pointer_compositor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace semilive::publisher::infra::capture {
namespace {

std::uint8_t byte_value(const std::byte value) noexcept {
    return std::to_integer<std::uint8_t>(value);
}

std::expected<std::uint32_t, std::string> visible_shape_height(
    const DesktopPointerShape& shape) {
    if (shape.width == 0 || shape.height == 0) {
        return std::unexpected{"desktop pointer shape dimensions must be non-zero"};
    }

    if (shape.type == DesktopPointerShapeType::Monochrome) {
        if ((shape.height % 2U) != 0U) {
            return std::unexpected{
                "monochrome desktop pointer shape height must contain equal AND and XOR masks"};
        }
        return shape.height / 2U;
    }
    return shape.height;
}

std::expected<void, std::string> validate_layout(
    const contracts::capture::DesktopImage& image,
    const DesktopPointerShape& shape) {
    if (image.width == 0 || image.height == 0 ||
        image.width > std::numeric_limits<std::uint32_t>::max() / 4U ||
        image.stride < image.width * 4U) {
        return std::unexpected{"desktop image has an invalid BGRA layout"};
    }

    const auto image_bytes =
        static_cast<std::uint64_t>(image.stride) * image.height;
    if (image_bytes > image.bgra.size()) {
        return std::unexpected{"desktop image buffer is smaller than its BGRA layout"};
    }

    const auto height_result = visible_shape_height(shape);
    if (!height_result) {
        return std::unexpected{height_result.error()};
    }

    const std::uint64_t minimum_pitch =
        shape.type == DesktopPointerShapeType::Monochrome
            ? (static_cast<std::uint64_t>(shape.width) + 7U) / 8U
            : static_cast<std::uint64_t>(shape.width) * 4U;
    if (shape.pitch < minimum_pitch) {
        return std::unexpected{"desktop pointer shape pitch is too small"};
    }

    const auto shape_bytes =
        static_cast<std::uint64_t>(shape.pitch) * shape.height;
    if (shape_bytes > shape.data.size()) {
        return std::unexpected{"desktop pointer shape buffer is smaller than its layout"};
    }
    return {};
}

std::uint8_t blend_channel(const std::uint8_t source,
                           const std::uint8_t destination,
                           const std::uint8_t alpha) noexcept {
    const auto source_part = static_cast<std::uint32_t>(source) * alpha;
    const auto destination_part =
        static_cast<std::uint32_t>(destination) * (255U - alpha);
    return static_cast<std::uint8_t>(
        (source_part + destination_part + 127U) / 255U);
}

void compose_color_pixel(std::byte* destination,
                         const std::byte* source) noexcept {
    const auto alpha = byte_value(source[3]);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        destination[channel] = static_cast<std::byte>(blend_channel(
            byte_value(source[channel]), byte_value(destination[channel]), alpha));
    }
}

void compose_masked_color_pixel(std::byte* destination,
                                const std::byte* source) noexcept {
    const auto mask = byte_value(source[3]);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto color = byte_value(source[channel]);
        destination[channel] = static_cast<std::byte>(
            mask == 0U ? color : byte_value(destination[channel]) ^ color);
    }
}

bool mask_bit(const std::byte* row, const std::uint32_t x) noexcept {
    const auto byte = byte_value(row[x / 8U]);
    const auto bit = 7U - (x % 8U);
    return ((byte >> bit) & 1U) != 0U;
}

void compose_monochrome_pixel(std::byte* destination,
                              const bool and_bit,
                              const bool xor_bit) noexcept {
    const auto and_mask = static_cast<std::uint8_t>(and_bit ? 0xFFU : 0U);
    const auto xor_mask = static_cast<std::uint8_t>(xor_bit ? 0xFFU : 0U);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        destination[channel] = static_cast<std::byte>(
            (byte_value(destination[channel]) & and_mask) ^ xor_mask);
    }
}

}  // namespace

std::expected<void, std::string> compose_desktop_pointer(
    contracts::capture::DesktopImage& image,
    const DesktopPointerShape& shape,
    const DesktopPointerPosition position) {
    const auto validation = validate_layout(image, shape);
    if (!validation) {
        return validation;
    }

    if (shape.type == DesktopPointerShapeType::MaskedColor) {
        for (std::uint32_t y = 0; y < shape.height; ++y) {
            const auto* row = shape.data.data() +
                              static_cast<std::size_t>(y) * shape.pitch;
            for (std::uint32_t x = 0; x < shape.width; ++x) {
                const auto alpha = byte_value(row[static_cast<std::size_t>(x) * 4U + 3U]);
                if (alpha != 0U && alpha != 0xFFU) {
                    return std::unexpected{
                        "masked-color desktop pointer alpha must be 0 or 255"};
                }
            }
        }
    }

    const auto pointer_height = *visible_shape_height(shape);
    const auto left = std::max<std::int64_t>(position.x, 0);
    const auto top = std::max<std::int64_t>(position.y, 0);
    const auto right = std::min<std::int64_t>(
        static_cast<std::int64_t>(position.x) + shape.width, image.width);
    const auto bottom = std::min<std::int64_t>(
        static_cast<std::int64_t>(position.y) + pointer_height, image.height);
    if (left >= right || top >= bottom) {
        return {};
    }

    const auto source_x_begin = static_cast<std::uint32_t>(left - position.x);
    const auto source_y_begin = static_cast<std::uint32_t>(top - position.y);
    const auto mask_plane_bytes =
        static_cast<std::size_t>(shape.pitch) * pointer_height;

    for (std::int64_t destination_y = top; destination_y < bottom; ++destination_y) {
        const auto source_y = source_y_begin +
                              static_cast<std::uint32_t>(destination_y - top);
        for (std::int64_t destination_x = left; destination_x < right; ++destination_x) {
            const auto source_x = source_x_begin +
                                  static_cast<std::uint32_t>(destination_x - left);
            auto* destination = image.bgra.data() +
                static_cast<std::size_t>(destination_y) * image.stride +
                static_cast<std::size_t>(destination_x) * 4U;

            if (shape.type == DesktopPointerShapeType::Monochrome) {
                const auto* and_row = shape.data.data() +
                    static_cast<std::size_t>(source_y) * shape.pitch;
                const auto* xor_row = and_row + mask_plane_bytes;
                compose_monochrome_pixel(
                    destination,
                    mask_bit(and_row, source_x),
                    mask_bit(xor_row, source_x));
                continue;
            }

            const auto* source = shape.data.data() +
                static_cast<std::size_t>(source_y) * shape.pitch +
                static_cast<std::size_t>(source_x) * 4U;
            if (shape.type == DesktopPointerShapeType::Color) {
                compose_color_pixel(destination, source);
            } else {
                compose_masked_color_pixel(destination, source);
            }
        }
    }
    return {};
}

}  // namespace semilive::publisher::infra::capture
