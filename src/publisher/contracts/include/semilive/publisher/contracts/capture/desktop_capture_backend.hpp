#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <variant>
#include <vector>

namespace semilive::publisher::contracts::capture {

enum class DesktopOutputSelection : std::uint8_t {
    Primary,
    Index,
};

struct DesktopOutputSelector {
    DesktopOutputSelection selection = DesktopOutputSelection::Primary;
    std::uint32_t index = 0;
};

struct DesktopCaptureConfig {
    DesktopOutputSelector output;
    bool compose_pointer = true;
};

struct DesktopCaptureInfo {
    std::string output_name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct DesktopImage {
    std::vector<std::byte> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
};

enum class DesktopCaptureOperation : std::uint8_t {
    Open,
    Acquire,
    Copy,
    Map,
    Pointer,
    Reinitialize,
};

struct DesktopCaptureIssue {
    DesktopCaptureOperation operation = DesktopCaptureOperation::Open;
    std::int64_t native_code = 0;
    std::string message;
};

struct DesktopNoChange {};

struct DesktopTemporarilyUnavailable {
    DesktopCaptureIssue issue;
};

using DesktopCaptureObservation =
    std::variant<DesktopImage, DesktopNoChange, DesktopTemporarilyUnavailable>;

using DesktopCaptureResult =
    std::expected<DesktopCaptureObservation, DesktopCaptureIssue>;

class DesktopCaptureBackend {
public:
    virtual ~DesktopCaptureBackend() = default;

    DesktopCaptureBackend(const DesktopCaptureBackend&) = delete;
    DesktopCaptureBackend& operator=(const DesktopCaptureBackend&) = delete;
    DesktopCaptureBackend(DesktopCaptureBackend&&) = delete;
    DesktopCaptureBackend& operator=(DesktopCaptureBackend&&) = delete;

    [[nodiscard]] virtual std::expected<DesktopCaptureInfo, DesktopCaptureIssue>
    open(const DesktopCaptureConfig& config) = 0;

    [[nodiscard]] virtual DesktopCaptureResult capture_latest() = 0;
    virtual void close() noexcept = 0;

protected:
    DesktopCaptureBackend() = default;
};

}  // namespace semilive::publisher::contracts::capture
