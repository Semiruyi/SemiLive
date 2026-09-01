#pragma once

#include "publisher/contracts/capture/desktop_capture_backend.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace semilive::publisher::infra::capture {

struct SyntheticDesktopCaptureFailure {
    contracts::capture::DesktopCaptureIssue issue;
};

using SyntheticDesktopCaptureStep =
    std::variant<contracts::capture::DesktopImage,
                 contracts::capture::DesktopNoChange,
                 contracts::capture::DesktopTemporarilyUnavailable,
                 SyntheticDesktopCaptureFailure>;

struct SyntheticDesktopCaptureScript {
    std::string output_name = "Synthetic Desktop";
    std::uint32_t initial_width = 1920;
    std::uint32_t initial_height = 1080;
    std::vector<SyntheticDesktopCaptureStep> steps;
};

class SyntheticDesktopCaptureBackend final
    : public contracts::capture::DesktopCaptureBackend {
public:
    explicit SyntheticDesktopCaptureBackend(SyntheticDesktopCaptureScript script);
    ~SyntheticDesktopCaptureBackend() override = default;

    [[nodiscard]] std::expected<contracts::capture::DesktopCaptureInfo,
                                contracts::capture::DesktopCaptureIssue>
    open(const contracts::capture::DesktopCaptureConfig& config) override;

    [[nodiscard]] contracts::capture::DesktopCaptureResult capture_latest() override;
    void close() noexcept override;

private:
    [[nodiscard]] std::optional<contracts::capture::DesktopCaptureIssue>
    validate_image(const contracts::capture::DesktopImage& image) const;

    SyntheticDesktopCaptureScript script_;
    std::size_t next_step_ = 0;
    bool open_ = false;
};

}  // namespace semilive::publisher::infra::capture
