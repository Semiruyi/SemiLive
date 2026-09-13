#pragma once

#include <semilive/receiver/domain/worker/video_receive_worker.hpp>

#include <memory>

namespace semilive::receiver::domain {

class DefaultVideoReceiveWorker final : public VideoReceiveWorker {
public:
    DefaultVideoReceiveWorker();
    ~DefaultVideoReceiveWorker() override;

    DefaultVideoReceiveWorker(const DefaultVideoReceiveWorker&) = delete;
    DefaultVideoReceiveWorker& operator=(const DefaultVideoReceiveWorker&) =
        delete;
    DefaultVideoReceiveWorker(DefaultVideoReceiveWorker&&) = delete;
    DefaultVideoReceiveWorker& operator=(DefaultVideoReceiveWorker&&) = delete;

    [[nodiscard]] VideoReceiveStartResult start() override;
    [[nodiscard]] VideoReceiveStopResult stop() override;

    [[nodiscard]] VideoReceiveWorkerState state() const noexcept override;
    [[nodiscard]] VideoReceiveWorkerStats stats() const noexcept override;
    [[nodiscard]] VideoReceiveWaitResult wait_for_terminal_for(
        std::chrono::milliseconds timeout) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::receiver::domain
