#include <semilive/publisher/domain/rtcp/default_publisher_rtcp_worker.hpp>

#include <semilive/common/rtcp/ntp_time.hpp>
#include <semilive/common/rtcp/rtcp_packet.hpp>
#include <semilive/publisher/domain/rtcp/publisher_rtcp_worker_events.hpp>
#include <semilive/publisher/domain/rtp/rtp_sender_state.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

namespace semilive::publisher::domain {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] PublisherRtcpWorkerIssue worker_issue(
    const PublisherRtcpWorkerOperation operation,
    std::string message) {
    return {operation, std::nullopt, std::move(message)};
}

[[nodiscard]] PublisherRtcpWorkerIssue transport_issue(
    const PublisherRtcpWorkerOperation operation,
    common::rtcp::TransportIssue issue) {
    auto message = issue.message;
    return {operation, std::move(issue), std::move(message)};
}

[[nodiscard]] std::string canonical_name(const std::uint32_t ssrc) {
    std::ostringstream output;
    output << "semilive-publisher@" << std::hex << std::setfill('0')
           << std::setw(8) << ssrc;
    return output.str();
}

[[nodiscard]] std::uint32_t extrapolated_timestamp(
    const RtpSenderSnapshot& snapshot,
    const Clock::time_point now,
    const std::uint32_t clock_rate) noexcept {
    const auto elapsed = std::max(Clock::duration::zero(),
                                  now - snapshot.last_sent_at);
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;
    const auto seconds = nanoseconds / nanoseconds_per_second;
    const auto remainder = nanoseconds % nanoseconds_per_second;
    const auto ticks =
        static_cast<std::uint64_t>(seconds) * clock_rate +
        static_cast<std::uint64_t>(remainder) * clock_rate /
            static_cast<std::uint64_t>(nanoseconds_per_second);
    return snapshot.last_rtp_timestamp + static_cast<std::uint32_t>(ticks);
}

}  // namespace

struct DefaultPublisherRtcpWorker::Impl {
    Impl(PublisherRtcpWorkerConfig config,
         std::unique_ptr<common::rtcp::Transport> transport,
         std::shared_ptr<RtpSenderState> sender_state,
         std::shared_ptr<contracts::Notifier> notifier)
        : config{std::move(config)},
          transport{std::move(transport)},
          sender_state{std::move(sender_state)},
          notifier{std::move(notifier)} {
        if (!this->transport || !this->sender_state || !this->notifier) {
            throw std::invalid_argument{
                "publisher RTCP worker dependencies must not be null"};
        }
    }

    ~Impl() {
        stop();
    }

    [[nodiscard]] PublisherRtcpStartResult start();
    void stop() noexcept;
    [[nodiscard]] PublisherRtcpWorkerState state() const noexcept;
    [[nodiscard]] PublisherRtcpWorkerStats stats() const noexcept;

    void run(std::stop_token stop_token) noexcept;
    [[nodiscard]] bool send_sender_report(Clock::time_point now);
    [[nodiscard]] bool receive_once(std::chrono::milliseconds timeout);
    void process_receiver_report(const common::rtcp::ReceiverReport& report,
                                 Clock::time_point received_at);
    void fail(PublisherRtcpWorkerIssue issue) noexcept;

    PublisherRtcpWorkerConfig config;
    std::unique_ptr<common::rtcp::Transport> transport;
    std::shared_ptr<RtpSenderState> sender_state;
    std::shared_ptr<contracts::Notifier> notifier;
    mutable std::mutex mutex;
    PublisherRtcpWorkerStats worker_stats;
    std::deque<std::uint32_t> sent_sender_reports;
    std::jthread thread;
};

PublisherRtcpStartResult DefaultPublisherRtcpWorker::Impl::start() {
    {
        std::lock_guard lock{mutex};
        if (worker_stats.state != PublisherRtcpWorkerState::Idle) {
            return std::unexpected{worker_issue(
                PublisherRtcpWorkerOperation::Control,
                "publisher RTCP worker can only start from idle")};
        }
    }
    if (config.report_interval <= std::chrono::milliseconds::zero() ||
        config.receive_poll_interval <= std::chrono::milliseconds::zero() ||
        config.rtp_clock_rate == 0U) {
        return std::unexpected{worker_issue(
            PublisherRtcpWorkerOperation::Control,
            "publisher RTCP intervals and RTP clock rate must be positive")};
    }

    auto opened = transport->open(config.transport);
    if (!opened) {
        return std::unexpected{transport_issue(
            PublisherRtcpWorkerOperation::OpenTransport,
            std::move(opened.error()))};
    }
    {
        std::lock_guard lock{mutex};
        worker_stats = {};
        worker_stats.state = PublisherRtcpWorkerState::Running;
        worker_stats.transport = *opened;
        sent_sender_reports.clear();
    }
    try {
        thread = std::jthread{
            [this](const std::stop_token token) { run(token); }};
    } catch (const std::exception& error) {
        transport->close();
        std::lock_guard lock{mutex};
        worker_stats.state = PublisherRtcpWorkerState::Idle;
        return std::unexpected{worker_issue(
            PublisherRtcpWorkerOperation::Internal, error.what())};
    }
    return *opened;
}

void DefaultPublisherRtcpWorker::Impl::stop() noexcept {
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
    transport->close();
    std::lock_guard lock{mutex};
    worker_stats.state = PublisherRtcpWorkerState::Idle;
}

PublisherRtcpWorkerState
DefaultPublisherRtcpWorker::Impl::state() const noexcept {
    std::lock_guard lock{mutex};
    return worker_stats.state;
}

PublisherRtcpWorkerStats
DefaultPublisherRtcpWorker::Impl::stats() const noexcept {
    std::lock_guard lock{mutex};
    return worker_stats;
}

void DefaultPublisherRtcpWorker::Impl::run(
    const std::stop_token stop_token) noexcept {
    auto next_report = Clock::now();
    while (!stop_token.stop_requested()) {
        const auto now = Clock::now();
        if (now >= next_report) {
            if (!send_sender_report(now)) {
                return;
            }
            next_report = now + config.report_interval;
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
            std::max(Clock::duration::zero(), next_report - Clock::now()));
        const auto timeout = std::min(config.receive_poll_interval, remaining);
        if (!receive_once(timeout)) {
            return;
        }
    }
}

bool DefaultPublisherRtcpWorker::Impl::send_sender_report(
    const Clock::time_point now) {
    const auto snapshot = sender_state->snapshot();
    if (!snapshot || !snapshot->has_sent_packet) {
        return true;
    }
    auto current_ntp = common::rtcp::ntp_timestamp(
        std::chrono::system_clock::now());
    if (!current_ntp) {
        fail(worker_issue(PublisherRtcpWorkerOperation::TimeConversion,
                          std::move(current_ntp.error())));
        return false;
    }

    common::rtcp::CompoundPacket compound{{
        common::rtcp::SenderReport{
            snapshot->ssrc,
            *current_ntp,
            extrapolated_timestamp(*snapshot, now, config.rtp_clock_rate),
            snapshot->packet_count,
            snapshot->payload_octet_count,
            {},
        },
        common::rtcp::SourceDescription{{
            {snapshot->ssrc, canonical_name(snapshot->ssrc)},
        }},
    }};
    auto serialized = common::rtcp::serialize_compound_packet(compound);
    if (!serialized) {
        fail(worker_issue(PublisherRtcpWorkerOperation::Serialize,
                          std::move(serialized.error())));
        return false;
    }
    if (auto sent = transport->send(*serialized); !sent) {
        fail(transport_issue(PublisherRtcpWorkerOperation::Send,
                             std::move(sent.error())));
        return false;
    }
    sent_sender_reports.push_back(common::rtcp::compact_ntp(*current_ntp));
    if (sent_sender_reports.size() > 16U) {
        sent_sender_reports.pop_front();
    }
    std::lock_guard lock{mutex};
    ++worker_stats.sender_reports_sent;
    return true;
}

bool DefaultPublisherRtcpWorker::Impl::receive_once(
    const std::chrono::milliseconds timeout) {
    auto received = transport->receive_for(timeout);
    if (!received) {
        fail(transport_issue(PublisherRtcpWorkerOperation::Receive,
                             std::move(received.error())));
        return false;
    }
    if (!*received) {
        return true;
    }
    auto parsed = common::rtcp::parse_compound_packet((*received)->bytes);
    if (!parsed) {
        std::lock_guard lock{mutex};
        ++worker_stats.invalid_packets;
        return true;
    }
    for (const auto& packet : parsed->packets) {
        const auto* report = std::get_if<common::rtcp::ReceiverReport>(&packet);
        if (report != nullptr) {
            process_receiver_report(*report, (*received)->received_at);
        }
    }
    return true;
}

void DefaultPublisherRtcpWorker::Impl::process_receiver_report(
    const common::rtcp::ReceiverReport& report,
    const Clock::time_point received_at) {
    static_cast<void>(received_at);
    const auto snapshot = sender_state->snapshot();
    std::lock_guard lock{mutex};
    ++worker_stats.receiver_reports_received;
    for (const auto& block : report.reports) {
        if (!snapshot || block.source_ssrc != snapshot->ssrc) {
            ++worker_stats.ignored_report_blocks;
            continue;
        }
        worker_stats.reported_fraction_lost = block.fraction_lost;
        worker_stats.reported_cumulative_lost = block.cumulative_lost;
        worker_stats.reported_jitter = block.interarrival_jitter;
        if (block.last_sender_report == 0U) {
            continue;
        }
        if (std::find(sent_sender_reports.begin(), sent_sender_reports.end(),
                      block.last_sender_report) ==
            sent_sender_reports.end()) {
            continue;
        }
        const auto current_ntp = common::rtcp::ntp_timestamp(
            std::chrono::system_clock::now());
        if (!current_ntp) {
            continue;
        }
        const auto arrival = common::rtcp::compact_ntp(*current_ntp);
        const auto compact_rtt = arrival - block.last_sender_report -
                                 block.delay_since_last_sender_report;
        constexpr std::uint32_t maximum_rtt = 60U << 16U;
        if (compact_rtt > maximum_rtt) {
            continue;
        }
        worker_stats.current_rtt =
            common::rtcp::duration_from_compact_ntp(compact_rtt);
        ++worker_stats.rtt_samples;
    }
}

void DefaultPublisherRtcpWorker::Impl::fail(
    PublisherRtcpWorkerIssue issue) noexcept {
    {
        std::lock_guard lock{mutex};
        worker_stats.state = PublisherRtcpWorkerState::Failed;
        worker_stats.last_issue = issue;
    }
    static_cast<void>(notifier->send(PublisherRtcpWorkerFailed{
        std::move(issue)}));
}

DefaultPublisherRtcpWorker::DefaultPublisherRtcpWorker(
    PublisherRtcpWorkerConfig config,
    std::unique_ptr<common::rtcp::Transport> transport,
    std::shared_ptr<RtpSenderState> sender_state,
    std::shared_ptr<contracts::Notifier> notifier)
    : impl_{std::make_unique<Impl>(
          std::move(config), std::move(transport), std::move(sender_state),
          std::move(notifier))} {}

DefaultPublisherRtcpWorker::~DefaultPublisherRtcpWorker() = default;

PublisherRtcpStartResult DefaultPublisherRtcpWorker::start() {
    return impl_->start();
}

void DefaultPublisherRtcpWorker::stop() noexcept {
    impl_->stop();
}

PublisherRtcpWorkerState DefaultPublisherRtcpWorker::state() const noexcept {
    return impl_->state();
}

PublisherRtcpWorkerStats DefaultPublisherRtcpWorker::stats() const noexcept {
    return impl_->stats();
}

}  // namespace semilive::publisher::domain
