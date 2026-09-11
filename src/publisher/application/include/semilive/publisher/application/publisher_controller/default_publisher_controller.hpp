#pragma once

#include <semilive/publisher/application/publisher_controller/publisher_controller.hpp>
#include <semilive/publisher/contracts/notifier/notifier.hpp>

#include <memory>

namespace semilive::publisher::application {

class DefaultPublisherController final : public PublisherController {
public:
    DefaultPublisherController(
        PublisherVideoSessionPlan plan,
        PublisherVideoPipeline pipeline,
        std::shared_ptr<contracts::Notifier> notifier);
    ~DefaultPublisherController() override;

    DefaultPublisherController(const DefaultPublisherController&) = delete;
    DefaultPublisherController& operator=(const DefaultPublisherController&) = delete;
    DefaultPublisherController(DefaultPublisherController&&) = delete;
    DefaultPublisherController& operator=(DefaultPublisherController&&) = delete;

    [[nodiscard]] PublisherStartResult start_publishing() override;
    [[nodiscard]] PublisherStopResult stop_publishing() override;

    [[nodiscard]] PublisherControllerState state() const noexcept override;
    [[nodiscard]] PublisherControllerStats stats() const noexcept override;
    [[nodiscard]] PublisherWaitResult wait_for_terminal_for(
        std::chrono::milliseconds timeout) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::publisher::application
