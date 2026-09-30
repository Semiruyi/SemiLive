#pragma once

#include <semilive/receiver/domain/rtcp/receiver_rtcp_worker.hpp>

#include <memory>

namespace semilive::receiver::domain {

class RtpReceptionStatistics;
class RtpMissingTracker;

class DefaultReceiverRtcpWorker final : public ReceiverRtcpWorker {
public:
    DefaultReceiverRtcpWorker(
        ReceiverRtcpWorkerConfig config,
        std::unique_ptr<common::rtcp::Transport> transport,
        std::shared_ptr<RtpReceptionStatistics> reception_statistics,
        std::shared_ptr<RtpMissingTracker> missing_tracker);
    ~DefaultReceiverRtcpWorker() override;

    DefaultReceiverRtcpWorker(const DefaultReceiverRtcpWorker&) = delete;
    DefaultReceiverRtcpWorker& operator=(const DefaultReceiverRtcpWorker&) =
        delete;
    DefaultReceiverRtcpWorker(DefaultReceiverRtcpWorker&&) = delete;
    DefaultReceiverRtcpWorker& operator=(DefaultReceiverRtcpWorker&&) = delete;

    [[nodiscard]] ReceiverRtcpStartResult start() override;
    [[nodiscard]] bool request_pli(
        std::uint32_t media_source_ssrc) noexcept override;
    void stop() noexcept override;
    [[nodiscard]] ReceiverRtcpWorkerState state() const noexcept override;
    [[nodiscard]] ReceiverRtcpWorkerStats stats() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::receiver::domain
