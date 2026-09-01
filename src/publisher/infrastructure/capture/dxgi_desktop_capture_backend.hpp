#pragma once

#include "publisher/contracts/capture/desktop_capture_backend.hpp"

#include <memory>

namespace semilive::publisher::infra::capture {

class DxgiDesktopCaptureBackend final
    : public contracts::capture::DesktopCaptureBackend {
public:
    DxgiDesktopCaptureBackend();
    ~DxgiDesktopCaptureBackend() override;

    [[nodiscard]] std::expected<contracts::capture::DesktopCaptureInfo,
                                contracts::capture::DesktopCaptureIssue>
    open(const contracts::capture::DesktopCaptureConfig& config) override;

    [[nodiscard]] contracts::capture::DesktopCaptureResult capture_latest() override;
    void close() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::publisher::infra::capture
