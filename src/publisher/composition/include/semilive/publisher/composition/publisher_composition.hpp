#pragma once

#include <semilive/publisher/application/publisher_controller/publisher_controller.hpp>
#include <semilive/publisher/composition/publisher_config.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace semilive::publisher::composition {

enum class PublisherCompositionOperation {
    Control,
    ValidateConfig,
    CheckPlatform,
    CreateNotifier,
    CreateResources,
    CreateOutputWorker,
    CreateEncoderWorker,
    CreateCaptureWorker,
    CreateController,
    StopSession,
    DisposeGraph,
};

struct PublisherCompositionIssue {
    PublisherCompositionOperation operation =
        PublisherCompositionOperation::Control;
    std::optional<application::PublisherControllerIssue> controller_issue;
    std::string message;
};

using PublisherCompositionResult =
    std::expected<void, PublisherCompositionIssue>;

class PublisherComposition final {
public:
    explicit PublisherComposition(PublisherConfig config);
    ~PublisherComposition();

    PublisherComposition(const PublisherComposition&) = delete;
    PublisherComposition& operator=(const PublisherComposition&) = delete;
    PublisherComposition(PublisherComposition&&) = delete;
    PublisherComposition& operator=(PublisherComposition&&) = delete;

    [[nodiscard]] PublisherCompositionResult assemble();

    [[nodiscard]] application::PublisherController* controller() noexcept;
    [[nodiscard]] const application::PublisherController*
    controller() const noexcept;

    [[nodiscard]] PublisherCompositionResult dispose();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::publisher::composition
