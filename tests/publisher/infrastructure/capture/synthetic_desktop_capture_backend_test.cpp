#include "publisher/contracts/capture/desktop_capture_backend.hpp"
#include "publisher/infrastructure/capture/synthetic_desktop_capture_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace capture_contract = semilive::publisher::contracts::capture;
namespace capture_infra = semilive::publisher::infra::capture;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename Exception, typename Operation>
void require_throws(Operation&& operation, const std::string_view message) {
    try {
        std::forward<Operation>(operation)();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error{std::string{message}};
}

capture_contract::DesktopImage image(const std::uint32_t width,
                                     const std::uint32_t height,
                                     const std::byte value) {
    const auto stride = width * 4U;
    return capture_contract::DesktopImage{
        std::vector<std::byte>(static_cast<std::size_t>(stride) * height, value),
        width,
        height,
        stride,
    };
}

capture_infra::SyntheticDesktopCaptureScript scripted_capture() {
    using capture_contract::DesktopCaptureIssue;
    using capture_contract::DesktopCaptureOperation;
    using capture_contract::DesktopNoChange;
    using capture_contract::DesktopTemporarilyUnavailable;
    using capture_infra::SyntheticDesktopCaptureFailure;

    capture_infra::SyntheticDesktopCaptureScript script;
    script.output_name = "Synthetic Test Output";
    script.initial_width = 2;
    script.initial_height = 1;
    script.steps.emplace_back(image(2, 1, std::byte{0x11}));
    script.steps.emplace_back(DesktopNoChange{});
    script.steps.emplace_back(DesktopTemporarilyUnavailable{
        DesktopCaptureIssue{
            DesktopCaptureOperation::Reinitialize,
            101,
            "synthetic output is temporarily unavailable",
        }});
    script.steps.emplace_back(SyntheticDesktopCaptureFailure{
        DesktopCaptureIssue{
            DesktopCaptureOperation::Acquire,
            202,
            "synthetic fatal capture failure",
        }});
    return script;
}

void backend_enforces_lifecycle_and_output_selection() {
    capture_infra::SyntheticDesktopCaptureBackend backend{scripted_capture()};

    const auto before_open = backend.capture_latest();
    require(!before_open &&
                before_open.error().operation == capture_contract::DesktopCaptureOperation::Acquire,
            "capture before open must fail as an acquire operation");

    capture_contract::DesktopCaptureConfig invalid_config;
    invalid_config.output.selection = capture_contract::DesktopOutputSelection::Index;
    invalid_config.output.index = 1;
    const auto invalid_open = backend.open(invalid_config);
    require(!invalid_open &&
                invalid_open.error().operation == capture_contract::DesktopCaptureOperation::Open,
            "an unavailable synthetic output index must be rejected");

    capture_contract::DesktopCaptureConfig config;
    config.output.selection = capture_contract::DesktopOutputSelection::Index;
    config.output.index = 0;
    const auto opened = backend.open(config);
    require(opened.has_value(), "synthetic output zero must open");
    require(opened->output_name == "Synthetic Test Output" &&
                opened->width == 2 && opened->height == 1,
            "open must report the scripted output identity and dimensions");

    const auto duplicate_open = backend.open(config);
    require(!duplicate_open &&
                duplicate_open.error().operation == capture_contract::DesktopCaptureOperation::Open,
            "opening an already open backend must fail");

    backend.close();
    backend.close();
    require(backend.open({}).has_value(), "close must be idempotent and permit reopening");
}

void backend_replays_every_observation_kind_in_order() {
    capture_infra::SyntheticDesktopCaptureBackend backend{scripted_capture()};
    require(backend.open({}).has_value(), "scripted backend must open");

    const auto first = backend.capture_latest();
    require(first && std::holds_alternative<capture_contract::DesktopImage>(*first),
            "first scripted step must return an image");
    const auto& first_image = std::get<capture_contract::DesktopImage>(*first);
    require(first_image.width == 2 && first_image.height == 1 &&
                first_image.stride == 8 && first_image.bgra.size() == 8,
            "synthetic image must retain its tightly packed BGRA layout");
    require(first_image.bgra.front() == std::byte{0x11},
            "synthetic image pixels must be deterministic");

    const auto second = backend.capture_latest();
    require(second && std::holds_alternative<capture_contract::DesktopNoChange>(*second),
            "second scripted step must report no change");

    const auto third = backend.capture_latest();
    require(third &&
                std::holds_alternative<capture_contract::DesktopTemporarilyUnavailable>(*third),
            "third scripted step must report temporary unavailability");
    const auto& temporary =
        std::get<capture_contract::DesktopTemporarilyUnavailable>(*third);
    require(temporary.issue.operation ==
                capture_contract::DesktopCaptureOperation::Reinitialize &&
                temporary.issue.native_code == 101,
            "temporary result must preserve its operation and native code");

    const auto fourth = backend.capture_latest();
    require(!fourth &&
                fourth.error().operation == capture_contract::DesktopCaptureOperation::Acquire &&
                fourth.error().native_code == 202,
            "fatal scripted step must use the expected error channel");

    const auto exhausted = backend.capture_latest();
    require(exhausted &&
                std::holds_alternative<capture_contract::DesktopNoChange>(*exhausted),
            "an exhausted script must settle on no change");
}

void reopening_restarts_the_script_with_independent_image_storage() {
    capture_infra::SyntheticDesktopCaptureBackend backend{scripted_capture()};
    require(backend.open({}).has_value(), "scripted backend must open");

    auto first = backend.capture_latest();
    require(first && std::holds_alternative<capture_contract::DesktopImage>(*first),
            "first session must produce the first image");
    std::get<capture_contract::DesktopImage>(*first).bgra.front() = std::byte{0x7f};

    backend.close();
    require(backend.open({}).has_value(), "backend must reopen for a new session");
    const auto replayed = backend.capture_latest();
    require(replayed && std::holds_alternative<capture_contract::DesktopImage>(*replayed),
            "reopened backend must restart its script");
    require(std::get<capture_contract::DesktopImage>(*replayed).bgra.front() ==
                std::byte{0x11},
            "returned images must not alias the backend script storage");
}

void malformed_images_and_invalid_scripts_are_rejected() {
    require_throws<std::invalid_argument>(
        [] {
            capture_infra::SyntheticDesktopCaptureScript script;
            script.output_name.clear();
            [[maybe_unused]] capture_infra::SyntheticDesktopCaptureBackend backend{
                std::move(script)};
        },
        "empty synthetic output name must be rejected");
    require_throws<std::invalid_argument>(
        [] {
            capture_infra::SyntheticDesktopCaptureScript script;
            script.initial_width = 0;
            [[maybe_unused]] capture_infra::SyntheticDesktopCaptureBackend backend{
                std::move(script)};
        },
        "zero synthetic output dimensions must be rejected");

    capture_infra::SyntheticDesktopCaptureScript script;
    script.initial_width = 1;
    script.initial_height = 1;
    auto malformed = image(1, 1, std::byte{0x22});
    malformed.stride = 8;
    script.steps.emplace_back(std::move(malformed));
    capture_infra::SyntheticDesktopCaptureBackend backend{std::move(script)};
    require(backend.open({}).has_value(), "backend with a malformed step must still open");

    const auto result = backend.capture_latest();
    require(!result &&
                result.error().operation == capture_contract::DesktopCaptureOperation::Copy,
            "malformed scripted image must become a fatal copy error");
}

}  // namespace

int main() {
    try {
        backend_enforces_lifecycle_and_output_selection();
        backend_replays_every_observation_kind_in_order();
        reopening_restarts_the_script_with_independent_image_storage();
        malformed_images_and_invalid_scripts_are_rejected();
    } catch (const std::exception& error) {
        std::cerr << "publisher synthetic capture test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher synthetic capture tests passed\n";
    return EXIT_SUCCESS;
}
