#pragma once

#include <semilive/receiver/domain/worker/video_receive_worker.hpp>

#include <chrono>

namespace semilive::receiver::application {

using ReceiverControllerState = domain::VideoReceiveWorkerState;
using ReceiverControllerIssue = domain::VideoReceiveWorkerIssue;
using ReceiverStarted = domain::VideoReceiveStarted;
using ReceiverStopped = domain::VideoReceiveStopped;
using ReceiverControllerStats = domain::VideoReceiveWorkerStats;
using ReceiverWaitStatus = domain::VideoReceiveWaitStatus;
using ReceiverWaitResult = domain::VideoReceiveWaitResult;
using ReceiverStartResult = domain::VideoReceiveStartResult;
using ReceiverStopResult = domain::VideoReceiveStopResult;

class ReceiverController {
public:
    virtual ~ReceiverController() = default;

    ReceiverController(const ReceiverController&) = delete;
    ReceiverController& operator=(const ReceiverController&) = delete;
    ReceiverController(ReceiverController&&) = delete;
    ReceiverController& operator=(ReceiverController&&) = delete;

    [[nodiscard]] virtual ReceiverStartResult start_receiving() = 0;
    [[nodiscard]] virtual ReceiverStopResult stop_receiving() = 0;

    [[nodiscard]] virtual ReceiverControllerState state() const noexcept = 0;
    [[nodiscard]] virtual ReceiverControllerStats stats() const noexcept = 0;
    [[nodiscard]] virtual ReceiverWaitResult wait_for_terminal_for(
        std::chrono::milliseconds timeout) = 0;

protected:
    ReceiverController() = default;
};

}  // namespace semilive::receiver::application
