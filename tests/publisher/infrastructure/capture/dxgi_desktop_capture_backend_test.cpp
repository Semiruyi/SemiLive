#include "publisher/contracts/capture/desktop_capture_backend.hpp"
#include "publisher/infrastructure/capture/dxgi_desktop_capture_backend.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

namespace capture_contract = semilive::publisher::contracts::capture;
using semilive::publisher::infra::capture::DxgiDesktopCaptureBackend;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

void backend_enforces_device_independent_lifecycle_rules() {
    DxgiDesktopCaptureBackend backend;

    const auto before_open = backend.capture_latest();
    require(!before_open &&
                before_open.error().operation ==
                    capture_contract::DesktopCaptureOperation::Acquire,
            "DXGI capture before open must fail as an acquire operation");

    capture_contract::DesktopCaptureConfig invalid_config;
    invalid_config.output.selection =
        static_cast<capture_contract::DesktopOutputSelection>(255);
    const auto invalid_open = backend.open(invalid_config);
    require(!invalid_open &&
                invalid_open.error().operation ==
                    capture_contract::DesktopCaptureOperation::Open,
            "DXGI backend must reject an invalid output selection before device access");

    backend.close();
    backend.close();
}

void backend_captures_an_interactive_desktop_frame() {
    DxgiDesktopCaptureBackend backend;
    capture_contract::DesktopCaptureConfig config;

    const auto opened = backend.open(config);
    require(opened.has_value(),
            opened ? "DXGI backend must open" : opened.error().message);
    require(opened->width > 0 && opened->height > 0,
            "opened DXGI output must report non-zero dimensions");

    const auto captured = backend.capture_latest();
    require(captured.has_value(),
            captured ? "DXGI capture must succeed" : captured.error().message);
    require(std::holds_alternative<capture_contract::DesktopImage>(*captured),
            "the first interactive DXGI capture must produce a desktop image");

    const auto& image = std::get<capture_contract::DesktopImage>(*captured);
    require(image.width == opened->width && image.height == opened->height,
            "captured DXGI dimensions must match the opened output");
    require(image.stride == image.width * 4U,
            "captured DXGI image must use tightly packed BGRA rows");
    require(image.bgra.size() ==
                static_cast<std::size_t>(image.stride) * image.height,
            "captured DXGI buffer size must match its layout");
    backend.close();
}

}  // namespace

int main(const int argc, char* argv[]) {
    try {
        backend_enforces_device_independent_lifecycle_rules();
        if (argc == 2 && std::string_view{argv[1]} == "--integration") {
            backend_captures_an_interactive_desktop_frame();
        }
    } catch (const std::exception& error) {
        std::cerr << "publisher DXGI capture test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher DXGI capture tests passed\n";
    return EXIT_SUCCESS;
}
