#pragma once

#include <semilive/receiver/application/receiver_controller.hpp>

#include <mutex>

namespace semilive::receiver::application {

class DefaultReceiverController final : public ReceiverController {
public:
    explicit DefaultReceiverController(domain::VideoReceiveWorker& worker);

    [[nodiscard]] ReceiverStartResult start_receiving() override;
    [[nodiscard]] ReceiverStopResult stop_receiving() override;

    [[nodiscard]] ReceiverControllerState state() const noexcept override;
    [[nodiscard]] ReceiverControllerStats stats() const noexcept override;
    [[nodiscard]] ReceiverWaitResult wait_for_terminal_for(
        std::chrono::milliseconds timeout) override;

private:
    domain::VideoReceiveWorker* worker_ = nullptr;
    std::mutex control_mutex_;
};

}  // namespace semilive::receiver::application
