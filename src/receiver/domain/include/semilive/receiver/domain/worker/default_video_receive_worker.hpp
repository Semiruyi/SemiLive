#pragma once

#include <semilive/receiver/contracts/network/datagram_source_backend.hpp>
#include <semilive/receiver/contracts/output/live_video_output_backend.hpp>
#include <semilive/receiver/domain/pipeline/h264_rtp_receive_pipeline.hpp>
#include <semilive/receiver/domain/worker/video_receive_worker.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace semilive::receiver::domain {

struct DefaultVideoReceiveWorkerConfig {
    contracts::network::DatagramSourceConfig input;
    std::chrono::milliseconds receive_poll_interval{10};
    std::chrono::milliseconds output_stall_threshold{100};
};

using DefaultVideoReceiveWorkerConfigValidationResult =
    std::expected<void, std::string>;

[[nodiscard]] DefaultVideoReceiveWorkerConfigValidationResult
validate_default_video_receive_worker_config(
    const DefaultVideoReceiveWorkerConfig& config);

class DefaultVideoReceiveWorker final : public VideoReceiveWorker {
public:
    DefaultVideoReceiveWorker(
        DefaultVideoReceiveWorkerConfig config,
        std::unique_ptr<contracts::network::DatagramSourceBackend> input,
        std::unique_ptr<H264RtpReceivePipeline> pipeline,
        std::unique_ptr<contracts::output::LiveVideoOutputBackend> output);
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
