#include <semilive/receiver/application/default_receiver_controller.hpp>

namespace semilive::receiver::application {

DefaultReceiverController::DefaultReceiverController(
    domain::VideoReceiveWorker& worker)
    : worker_{&worker} {}

ReceiverStartResult DefaultReceiverController::start_receiving() {
    std::lock_guard lock{control_mutex_};
    return worker_->start();
}

ReceiverStopResult DefaultReceiverController::stop_receiving() {
    std::lock_guard lock{control_mutex_};
    return worker_->stop();
}

ReceiverControllerState DefaultReceiverController::state() const noexcept {
    return worker_->state();
}

ReceiverControllerStats DefaultReceiverController::stats() const noexcept {
    return worker_->stats();
}

ReceiverWaitResult DefaultReceiverController::wait_for_terminal_for(
    const std::chrono::milliseconds timeout) {
    return worker_->wait_for_terminal_for(timeout);
}

}  // namespace semilive::receiver::application
