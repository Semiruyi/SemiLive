#pragma once

#include "publisher/contracts/capture/desktop_capture_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace semilive::publisher::infra::capture {

enum class DesktopPointerShapeType : std::uint8_t {
    Color,
    Monochrome,
    MaskedColor,
};

struct DesktopPointerShape {
    DesktopPointerShapeType type = DesktopPointerShapeType::Color;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0;
    std::vector<std::byte> data;
};

struct DesktopPointerPosition {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

[[nodiscard]] std::expected<void, std::string> compose_desktop_pointer(
    contracts::capture::DesktopImage& image,
    const DesktopPointerShape& shape,
    DesktopPointerPosition position);

}  // namespace semilive::publisher::infra::capture
