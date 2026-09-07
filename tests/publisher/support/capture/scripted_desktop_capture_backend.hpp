#pragma once

#include <semilive/publisher/contracts/capture/desktop_capture_backend.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace semilive::publisher::test_support::capture {

struct ScriptedDesktopCaptureFailure {
    contracts::capture::DesktopCaptureIssue issue;
};

using ScriptedDesktopCaptureStep =
    std::variant<contracts::capture::DesktopImage,
                 contracts::capture::DesktopNoChange,
                 contracts::capture::DesktopTemporarilyUnavailable,
                 ScriptedDesktopCaptureFailure>;

struct ScriptedDesktopCaptureScript {
    std::string output_name = "Scripted Desktop";
    std::uint32_t initial_width = 1920;
    std::uint32_t initial_height = 1080;
    std::vector<ScriptedDesktopCaptureStep> steps;
};

class ScriptedDesktopCaptureBackend final
    : public contracts::capture::DesktopCaptureBackend {
public:
    explicit ScriptedDesktopCaptureBackend(ScriptedDesktopCaptureScript script);
    ~ScriptedDesktopCaptureBackend() override = default;

    [[nodiscard]] std::expected<contracts::capture::DesktopCaptureInfo,
                                contracts::capture::DesktopCaptureIssue>
    open(const contracts::capture::DesktopCaptureConfig& config) override;
    [[nodiscard]] contracts::capture::DesktopCaptureResult capture_latest() override;
    void close() noexcept override;

private:
    [[nodiscard]] std::optional<contracts::capture::DesktopCaptureIssue>
    validate_image(const contracts::capture::DesktopImage& image) const;

    ScriptedDesktopCaptureScript script_;
    std::size_t next_step_ = 0;
    bool open_ = false;
};

}  // namespace semilive::publisher::test_support::capture
