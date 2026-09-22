#pragma once

#include <semilive/common/rtcp/rtcp_transport.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace semilive::receiver::domain {

struct ReceiverRtcpWorkerConfig {
    common::rtcp::TransportConfig transport;
    std::chrono::milliseconds report_interval{1000};
    std::chrono::milliseconds receive_poll_interval{20};
    std::optional<std::uint32_t> local_ssrc;
};

enum class ReceiverRtcpWorkerState : std::uint8_t {
    Idle,
    Running,
    Failed,
};

enum class ReceiverRtcpWorkerOperation : std::uint8_t {
    Control,
    OpenTransport,
    Receive,
    Serialize,
    Send,
    Internal,
};

struct ReceiverRtcpWorkerIssue {
    ReceiverRtcpWorkerOperation operation = ReceiverRtcpWorkerOperation::Internal;
    std::optional<common::rtcp::TransportIssue> transport_issue;
    std::string message;
};

struct ReceiverRtcpWorkerStats {
    ReceiverRtcpWorkerState state = ReceiverRtcpWorkerState::Idle;
    std::optional<common::rtcp::TransportInfo> transport;
    std::uint64_t sender_reports_received = 0;
    std::uint64_t receiver_reports_sent = 0;
    std::uint64_t invalid_packets = 0;
    std::uint64_t ignored_sender_reports = 0;
    std::optional<std::uint8_t> current_fraction_lost;
    std::optional<std::int32_t> cumulative_lost;
    std::optional<std::uint32_t> extended_highest_sequence;
    std::optional<std::uint32_t> interarrival_jitter;
    std::optional<std::uint32_t> last_sender_report;
    std::optional<std::uint32_t> delay_since_last_sender_report;
    std::optional<ReceiverRtcpWorkerIssue> last_issue;
};

using ReceiverRtcpStartResult =
    std::expected<common::rtcp::TransportInfo, ReceiverRtcpWorkerIssue>;

class ReceiverRtcpWorker {
public:
    virtual ~ReceiverRtcpWorker() = default;

    ReceiverRtcpWorker(const ReceiverRtcpWorker&) = delete;
    ReceiverRtcpWorker& operator=(const ReceiverRtcpWorker&) = delete;
    ReceiverRtcpWorker(ReceiverRtcpWorker&&) = delete;
    ReceiverRtcpWorker& operator=(ReceiverRtcpWorker&&) = delete;

    [[nodiscard]] virtual ReceiverRtcpStartResult start() = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual ReceiverRtcpWorkerState state() const noexcept = 0;
    [[nodiscard]] virtual ReceiverRtcpWorkerStats stats() const noexcept = 0;

protected:
    ReceiverRtcpWorker() = default;
};

}  // namespace semilive::receiver::domain
