#include "publisher/contracts/capture/desktop_capture_backend.hpp"
#include "publisher/infrastructure/capture/desktop_pointer_compositor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace capture_contract = semilive::publisher::contracts::capture;
using semilive::publisher::infra::capture::DesktopPointerPosition;
using semilive::publisher::infra::capture::DesktopPointerShape;
using semilive::publisher::infra::capture::DesktopPointerShapeType;
using semilive::publisher::infra::capture::compose_desktop_pointer;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

std::vector<std::byte> bytes(const std::initializer_list<std::uint8_t> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

capture_contract::DesktopImage image(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::array<std::uint8_t, 4>& pixel) {
    capture_contract::DesktopImage result;
    result.width = width;
    result.height = height;
    result.stride = width * 4U;
    result.bgra.resize(static_cast<std::size_t>(result.stride) * height);
    for (std::size_t offset = 0; offset < result.bgra.size(); offset += 4U) {
        for (std::size_t channel = 0; channel < pixel.size(); ++channel) {
            result.bgra[offset + channel] = static_cast<std::byte>(pixel[channel]);
        }
    }
    return result;
}

std::uint8_t channel(const capture_contract::DesktopImage& image,
                     const std::uint32_t x,
                     const std::uint32_t y,
                     const std::size_t index) {
    return std::to_integer<std::uint8_t>(
        image.bgra[static_cast<std::size_t>(y) * image.stride +
                   static_cast<std::size_t>(x) * 4U + index]);
}

void color_pointer_alpha_blends_and_clips() {
    auto desktop = image(2, 2, {10, 20, 30, 255});
    DesktopPointerShape shape;
    shape.type = DesktopPointerShapeType::Color;
    shape.width = 2;
    shape.height = 2;
    shape.pitch = 8;
    shape.data = bytes({
        110, 120, 130, 255, 210, 220, 230, 255,
        110, 120, 130, 128, 210, 220, 230, 0,
    });

    const auto result = compose_desktop_pointer(desktop, shape, {-1, 0});
    require(result.has_value(), "clipped color pointer composition must succeed");
    require(channel(desktop, 0, 0, 0) == 210 &&
                channel(desktop, 0, 0, 1) == 220 &&
                channel(desktop, 0, 0, 2) == 230,
            "opaque clipped color pixel must replace desktop RGB");
    require(channel(desktop, 0, 1, 0) == 10 &&
                channel(desktop, 0, 1, 1) == 20 &&
                channel(desktop, 0, 1, 2) == 30,
            "transparent clipped color pixel must preserve desktop RGB");
    require(channel(desktop, 1, 0, 0) == 10,
            "pixels outside the pointer must remain unchanged");
}

void color_pointer_uses_rounded_straight_alpha() {
    auto desktop = image(1, 1, {10, 20, 30, 255});
    DesktopPointerShape shape;
    shape.type = DesktopPointerShapeType::Color;
    shape.width = 1;
    shape.height = 1;
    shape.pitch = 4;
    shape.data = bytes({110, 120, 130, 128});

    const auto result = compose_desktop_pointer(desktop, shape, {0, 0});
    require(result.has_value(), "color alpha composition must succeed");
    require(channel(desktop, 0, 0, 0) == 60 &&
                channel(desktop, 0, 0, 1) == 70 &&
                channel(desktop, 0, 0, 2) == 80,
            "color pointer must use rounded straight-alpha blending");
    require(channel(desktop, 0, 0, 3) == 255,
            "pointer composition must preserve desktop alpha");
}

void monochrome_pointer_applies_and_xor_truth_table() {
    auto desktop = image(4, 1, {0x12, 0x34, 0x56, 255});
    DesktopPointerShape shape;
    shape.type = DesktopPointerShapeType::Monochrome;
    shape.width = 4;
    shape.height = 2;
    shape.pitch = 1;
    shape.data = bytes({0x30, 0x50});

    const auto result = compose_desktop_pointer(desktop, shape, {0, 0});
    require(result.has_value(), "monochrome pointer composition must succeed");
    require(channel(desktop, 0, 0, 0) == 0x00,
            "AND 0 XOR 0 must produce black");
    require(channel(desktop, 1, 0, 0) == 0xFF,
            "AND 0 XOR 1 must produce white");
    require(channel(desktop, 2, 0, 0) == 0x12,
            "AND 1 XOR 0 must preserve the desktop");
    require(channel(desktop, 3, 0, 0) == 0xED,
            "AND 1 XOR 1 must invert the desktop");
}

void masked_color_pointer_copies_or_xors_rgb() {
    auto desktop = image(2, 1, {0x0F, 0x33, 0x55, 255});
    DesktopPointerShape shape;
    shape.type = DesktopPointerShapeType::MaskedColor;
    shape.width = 2;
    shape.height = 1;
    shape.pitch = 8;
    shape.data = bytes({
        0x10, 0x20, 0x30, 0x00,
        0xF0, 0x0F, 0xAA, 0xFF,
    });

    const auto result = compose_desktop_pointer(desktop, shape, {0, 0});
    require(result.has_value(), "masked-color pointer composition must succeed");
    require(channel(desktop, 0, 0, 0) == 0x10 &&
                channel(desktop, 0, 0, 1) == 0x20 &&
                channel(desktop, 0, 0, 2) == 0x30,
            "zero masked-color alpha must copy pointer RGB");
    require(channel(desktop, 1, 0, 0) == 0xFF &&
                channel(desktop, 1, 0, 1) == 0x3C &&
                channel(desktop, 1, 0, 2) == 0xFF,
            "255 masked-color alpha must XOR pointer and desktop RGB");
}

void invalid_pointer_layouts_are_rejected() {
    auto desktop = image(1, 1, {0, 0, 0, 255});
    DesktopPointerShape odd_monochrome;
    odd_monochrome.type = DesktopPointerShapeType::Monochrome;
    odd_monochrome.width = 1;
    odd_monochrome.height = 1;
    odd_monochrome.pitch = 1;
    odd_monochrome.data = bytes({0});
    require(!compose_desktop_pointer(desktop, odd_monochrome, {0, 0}),
            "odd monochrome mask height must be rejected");

    DesktopPointerShape invalid_mask;
    invalid_mask.type = DesktopPointerShapeType::MaskedColor;
    invalid_mask.width = 1;
    invalid_mask.height = 1;
    invalid_mask.pitch = 4;
    invalid_mask.data = bytes({0, 0, 0, 128});
    require(!compose_desktop_pointer(desktop, invalid_mask, {0, 0}),
            "masked-color alpha other than 0 and 255 must be rejected");
}

}  // namespace

int main() {
    try {
        color_pointer_alpha_blends_and_clips();
        color_pointer_uses_rounded_straight_alpha();
        monochrome_pointer_applies_and_xor_truth_table();
        masked_color_pointer_copies_or_xors_rgb();
        invalid_pointer_layouts_are_rejected();
    } catch (const std::exception& error) {
        std::cerr << "publisher desktop pointer compositor tests failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher desktop pointer compositor tests passed\n";
    return EXIT_SUCCESS;
}
