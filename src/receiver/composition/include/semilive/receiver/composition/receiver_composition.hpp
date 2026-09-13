#pragma once

#include <semilive/receiver/application/receiver_controller.hpp>
#include <semilive/receiver/composition/receiver_config.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace semilive::receiver::composition {

enum class ReceiverCompositionOperation {
    Control,
    CreateWorker,
    CreateController,
    StopSession,
    DisposeGraph,
};

struct ReceiverCompositionIssue {
    ReceiverCompositionOperation operation =
        ReceiverCompositionOperation::Control;
    std::optional<application::ReceiverControllerIssue> controller_issue;
    std::string message;
};

using ReceiverCompositionResult =
    std::expected<void, ReceiverCompositionIssue>;

class ReceiverComposition final {
public:
    explicit ReceiverComposition(ReceiverConfig config = {});
    ~ReceiverComposition();

    ReceiverComposition(const ReceiverComposition&) = delete;
    ReceiverComposition& operator=(const ReceiverComposition&) = delete;
    ReceiverComposition(ReceiverComposition&&) = delete;
    ReceiverComposition& operator=(ReceiverComposition&&) = delete;

    [[nodiscard]] ReceiverCompositionResult assemble();

    [[nodiscard]] application::ReceiverController* controller() noexcept;
    [[nodiscard]] const application::ReceiverController*
    controller() const noexcept;

    [[nodiscard]] ReceiverCompositionResult dispose();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::receiver::composition
