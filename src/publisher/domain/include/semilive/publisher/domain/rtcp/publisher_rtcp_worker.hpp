#pragma once

#include <semilive/common/rtcp/rtcp_transport.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace semilive::publisher::domain {

struct PublisherRtcpWorkerConfig {
    common::rtcp::TransportConfig transport;
    std::chrono::milliseconds report_interval{1000};
    std::chrono::milliseconds receive_poll_interval{20};
    std::uint32_t rtp_clock_rate = 90'000;
};

enum class PublisherRtcpWorkerState : std::uint8_t {
    Idle,
    Running,
    Failed,
};

enum class PublisherRtcpWorkerOperation : std::uint8_t {
    Control,
    OpenTransport,
    Receive,
    Parse,
    Serialize,
    Send,
    TimeConversion,
    Internal,
};

struct PublisherRtcpWorkerIssue {
    PublisherRtcpWorkerOperation operation =
        PublisherRtcpWorkerOperation::Internal;
    std::optional<common::rtcp::TransportIssue> transport_issue;
    std::string message;
};

struct PublisherRtcpWorkerStats {
    PublisherRtcpWorkerState state = PublisherRtcpWorkerState::Idle;
    std::optional<common::rtcp::TransportInfo> transport;
    std::uint64_t sender_reports_sent = 0;
    std::uint64_t receiver_reports_received = 0;
    std::uint64_t invalid_packets = 0;
    std::uint64_t ignored_report_blocks = 0;
    std::uint64_t rtt_samples = 0;
    std::optional<std::chrono::nanoseconds> current_rtt;
    std::optional<std::uint8_t> reported_fraction_lost;
    std::optional<std::int32_t> reported_cumulative_lost;
    std::optional<std::uint32_t> reported_jitter;
    std::optional<PublisherRtcpWorkerIssue> last_issue;
};

using PublisherRtcpStartResult =
    std::expected<common::rtcp::TransportInfo, PublisherRtcpWorkerIssue>;

class PublisherRtcpWorker {
public:
    virtual ~PublisherRtcpWorker() = default;

    PublisherRtcpWorker(const PublisherRtcpWorker&) = delete;
    PublisherRtcpWorker& operator=(const PublisherRtcpWorker&) = delete;
    PublisherRtcpWorker(PublisherRtcpWorker&&) = delete;
    PublisherRtcpWorker& operator=(PublisherRtcpWorker&&) = delete;

    [[nodiscard]] virtual PublisherRtcpStartResult start() = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual PublisherRtcpWorkerState state() const noexcept = 0;
    [[nodiscard]] virtual PublisherRtcpWorkerStats stats() const noexcept = 0;

protected:
    PublisherRtcpWorker() = default;
};

}  // namespace semilive::publisher::domain
