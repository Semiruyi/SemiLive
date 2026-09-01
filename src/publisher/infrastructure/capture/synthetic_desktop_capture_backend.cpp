#include "publisher/infrastructure/capture/synthetic_desktop_capture_backend.hpp"

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace semilive::publisher::infra::capture {
namespace {

using contracts::capture::DesktopCaptureIssue;
using contracts::capture::DesktopCaptureObservation;
using contracts::capture::DesktopCaptureOperation;
using contracts::capture::DesktopCaptureResult;
using contracts::capture::DesktopImage;
using contracts::capture::DesktopNoChange;
using contracts::capture::DesktopOutputSelection;

DesktopCaptureIssue issue(const DesktopCaptureOperation operation,
                          std::string message) {
    return DesktopCaptureIssue{operation, 0, std::move(message)};
}

}  // namespace

SyntheticDesktopCaptureBackend::SyntheticDesktopCaptureBackend(
    SyntheticDesktopCaptureScript script)
    : script_{std::move(script)} {
    if (script_.output_name.empty()) {
        throw std::invalid_argument{"synthetic output name must not be empty"};
    }
    if (script_.initial_width == 0 || script_.initial_height == 0) {
        throw std::invalid_argument{"synthetic output dimensions must be non-zero"};
    }
}

std::expected<contracts::capture::DesktopCaptureInfo, DesktopCaptureIssue>
SyntheticDesktopCaptureBackend::open(
    const contracts::capture::DesktopCaptureConfig& config) {
    if (open_) {
        return std::unexpected{
            issue(DesktopCaptureOperation::Open,
                  "synthetic desktop capture backend is already open")};
    }
    if (config.output.selection == DesktopOutputSelection::Index &&
        config.output.index != 0) {
        return std::unexpected{
            issue(DesktopCaptureOperation::Open,
                  "synthetic desktop output index is out of range")};
    }

    next_step_ = 0;
    open_ = true;
    return contracts::capture::DesktopCaptureInfo{
        script_.output_name,
        script_.initial_width,
        script_.initial_height,
    };
}

DesktopCaptureResult SyntheticDesktopCaptureBackend::capture_latest() {
    if (!open_) {
        return std::unexpected{
            issue(DesktopCaptureOperation::Acquire,
                  "synthetic desktop capture backend is not open")};
    }
    if (next_step_ == script_.steps.size()) {
        return DesktopCaptureObservation{DesktopNoChange{}};
    }

    const auto& step = script_.steps[next_step_++];
    return std::visit(
        [this](const auto& value) -> DesktopCaptureResult {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, DesktopImage>) {
                if (auto validation_error = validate_image(value)) {
                    return std::unexpected{std::move(*validation_error)};
                }
                return DesktopCaptureObservation{value};
            } else if constexpr (std::is_same_v<Value, SyntheticDesktopCaptureFailure>) {
                return std::unexpected{value.issue};
            } else {
                return DesktopCaptureObservation{value};
            }
        },
        step);
}

void SyntheticDesktopCaptureBackend::close() noexcept {
    open_ = false;
    next_step_ = 0;
}

std::optional<DesktopCaptureIssue> SyntheticDesktopCaptureBackend::validate_image(
    const DesktopImage& image) const {
    if (image.width == 0 || image.height == 0) {
        return issue(DesktopCaptureOperation::Copy,
                     "synthetic desktop image dimensions must be non-zero");
    }
    if (image.width > std::numeric_limits<std::uint32_t>::max() / 4U) {
        return issue(DesktopCaptureOperation::Copy,
                     "synthetic desktop image stride exceeds the supported range");
    }

    const auto expected_stride = image.width * 4U;
    if (image.stride != expected_stride) {
        return issue(DesktopCaptureOperation::Copy,
                     "synthetic desktop image must use a tightly packed BGRA stride");
    }

    const auto expected_size =
        static_cast<std::uint64_t>(image.stride) * image.height;
    if (expected_size > std::numeric_limits<std::size_t>::max() ||
        image.bgra.size() != static_cast<std::size_t>(expected_size)) {
        return issue(DesktopCaptureOperation::Copy,
                     "synthetic desktop image buffer size does not match its layout");
    }
    return std::nullopt;
}

}  // namespace semilive::publisher::infra::capture
