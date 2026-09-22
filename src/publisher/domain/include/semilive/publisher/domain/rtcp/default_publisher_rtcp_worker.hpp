#pragma once

#include <semilive/publisher/contracts/notifier/notifier.hpp>
#include <semilive/publisher/domain/rtcp/publisher_rtcp_worker.hpp>

#include <memory>

namespace semilive::publisher::domain {

class RtpSenderState;

class DefaultPublisherRtcpWorker final : public PublisherRtcpWorker {
public:
    DefaultPublisherRtcpWorker(
        PublisherRtcpWorkerConfig config,
        std::unique_ptr<common::rtcp::Transport> transport,
        std::shared_ptr<RtpSenderState> sender_state,
        std::shared_ptr<contracts::Notifier> notifier);
    ~DefaultPublisherRtcpWorker() override;

    DefaultPublisherRtcpWorker(const DefaultPublisherRtcpWorker&) = delete;
    DefaultPublisherRtcpWorker& operator=(const DefaultPublisherRtcpWorker&) =
        delete;
    DefaultPublisherRtcpWorker(DefaultPublisherRtcpWorker&&) = delete;
    DefaultPublisherRtcpWorker& operator=(DefaultPublisherRtcpWorker&&) =
        delete;

    [[nodiscard]] PublisherRtcpStartResult start() override;
    void stop() noexcept override;
    [[nodiscard]] PublisherRtcpWorkerState state() const noexcept override;
    [[nodiscard]] PublisherRtcpWorkerStats stats() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::publisher::domain
