#include <semilive/receiver/domain/rtcp/default_receiver_rtcp_worker.hpp>

#include <semilive/common/rtcp/ntp_time.hpp>
#include <semilive/common/rtcp/rtcp_packet.hpp>
#include <semilive/receiver/domain/rtp/rtp_missing_tracker.hpp>
#include <semilive/receiver/domain/rtp/rtp_reception_statistics.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>

namespace semilive::receiver::domain {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] ReceiverRtcpWorkerIssue worker_issue(
    const ReceiverRtcpWorkerOperation operation,
    std::string message) {
    return {operation, std::nullopt, std::move(message)};
}

[[nodiscard]] ReceiverRtcpWorkerIssue transport_issue(
    const ReceiverRtcpWorkerOperation operation,
    common::rtcp::TransportIssue issue) {
    auto message = issue.message;
    return {operation, std::move(issue), std::move(message)};
}

[[nodiscard]] std::uint32_t random_ssrc() {
    std::random_device random;
    const auto high = static_cast<std::uint32_t>(random()) << 16U;
    const auto low = static_cast<std::uint32_t>(random()) & 0xffffU;
    return high | low;
}

[[nodiscard]] std::string canonical_name(const std::uint32_t ssrc) {
    std::ostringstream output;
    output << "semilive-receiver@" << std::hex << std::setfill('0')
           << std::setw(8) << ssrc;
    return output.str();
}

}  // namespace

struct DefaultReceiverRtcpWorker::Impl {
    Impl(ReceiverRtcpWorkerConfig config,
         std::unique_ptr<common::rtcp::Transport> transport,
         std::shared_ptr<RtpReceptionStatistics> reception_statistics,
         std::shared_ptr<RtpMissingTracker> missing_tracker)
        : config{std::move(config)},
          transport{std::move(transport)},
          reception_statistics{std::move(reception_statistics)},
          missing_tracker{std::move(missing_tracker)} {
        if (!this->transport || !this->reception_statistics ||
            !this->missing_tracker) {
            throw std::invalid_argument{
                "receiver RTCP worker dependencies must not be null"};
        }
    }

    ~Impl() {
        stop();
    }

    [[nodiscard]] ReceiverRtcpStartResult start();
    [[nodiscard]] bool request_pli(std::uint32_t media_source_ssrc) noexcept;
    void stop() noexcept;
    [[nodiscard]] ReceiverRtcpWorkerState state() const noexcept;
    [[nodiscard]] ReceiverRtcpWorkerStats stats() const noexcept;

    void run(std::stop_token stop_token) noexcept;
    [[nodiscard]] bool send_receiver_report(Clock::time_point now);
    [[nodiscard]] bool send_due_nacks(Clock::time_point now);
    [[nodiscard]] bool send_pending_pli();
    [[nodiscard]] bool receive_once(std::chrono::milliseconds timeout);
    void process_sender_report(const common::rtcp::SenderReport& report,
                               Clock::time_point received_at);
    void fail(ReceiverRtcpWorkerIssue issue) noexcept;

    ReceiverRtcpWorkerConfig config;
    std::unique_ptr<common::rtcp::Transport> transport;
    std::shared_ptr<RtpReceptionStatistics> reception_statistics;
    std::shared_ptr<RtpMissingTracker> missing_tracker;
    mutable std::mutex mutex;
    ReceiverRtcpWorkerStats worker_stats;
    std::uint32_t local_ssrc = 0;
    std::optional<std::uint32_t> last_sr;
    std::optional<Clock::time_point> last_sr_received_at;
    std::optional<std::uint32_t> pending_pli_source_ssrc;
    std::jthread thread;
};

ReceiverRtcpStartResult DefaultReceiverRtcpWorker::Impl::start() {
    {
        std::lock_guard lock{mutex};
        if (worker_stats.state != ReceiverRtcpWorkerState::Idle) {
            return std::unexpected{worker_issue(
                ReceiverRtcpWorkerOperation::Control,
                "receiver RTCP worker can only start from idle")};
        }
    }
    if (config.report_interval <= std::chrono::milliseconds::zero() ||
        config.receive_poll_interval <= std::chrono::milliseconds::zero()) {
        return std::unexpected{worker_issue(
            ReceiverRtcpWorkerOperation::Control,
            "receiver RTCP intervals must be positive")};
    }

    auto opened = transport->open(config.transport);
    if (!opened) {
        return std::unexpected{transport_issue(
            ReceiverRtcpWorkerOperation::OpenTransport,
            std::move(opened.error()))};
    }
    try {
        local_ssrc = config.local_ssrc.value_or(random_ssrc());
    } catch (const std::exception& error) {
        transport->close();
        return std::unexpected{worker_issue(
            ReceiverRtcpWorkerOperation::Internal, error.what())};
    }
    last_sr.reset();
    last_sr_received_at.reset();
    {
        std::lock_guard lock{mutex};
        worker_stats = {};
        worker_stats.state = ReceiverRtcpWorkerState::Running;
        worker_stats.transport = *opened;
        pending_pli_source_ssrc.reset();
    }
    try {
        thread = std::jthread{
            [this](const std::stop_token token) { run(token); }};
    } catch (const std::exception& error) {
        transport->close();
        std::lock_guard lock{mutex};
        worker_stats.state = ReceiverRtcpWorkerState::Idle;
        return std::unexpected{worker_issue(
            ReceiverRtcpWorkerOperation::Internal, error.what())};
    }
    return *opened;
}

void DefaultReceiverRtcpWorker::Impl::stop() noexcept {
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
    transport->close();
    std::lock_guard lock{mutex};
    worker_stats.state = ReceiverRtcpWorkerState::Idle;
    pending_pli_source_ssrc.reset();
}

bool DefaultReceiverRtcpWorker::Impl::request_pli(
    const std::uint32_t media_source_ssrc) noexcept {
    std::lock_guard lock{mutex};
    if (worker_stats.state != ReceiverRtcpWorkerState::Running) {
        return false;
    }
    if (pending_pli_source_ssrc) {
        if (*pending_pli_source_ssrc != media_source_ssrc) {
            return false;
        }
        ++worker_stats.pli_requests_coalesced;
        return true;
    }
    pending_pli_source_ssrc = media_source_ssrc;
    return true;
}

ReceiverRtcpWorkerState
DefaultReceiverRtcpWorker::Impl::state() const noexcept {
    std::lock_guard lock{mutex};
    return worker_stats.state;
}

ReceiverRtcpWorkerStats
DefaultReceiverRtcpWorker::Impl::stats() const noexcept {
    auto result = [&] {
        std::lock_guard lock{mutex};
        return worker_stats;
    }();
    result.missing_tracker = missing_tracker->stats();
    return result;
}

void DefaultReceiverRtcpWorker::Impl::run(
    const std::stop_token stop_token) noexcept {
    auto next_report = Clock::now();
    while (!stop_token.stop_requested()) {
        const auto now = Clock::now();
        if (now >= next_report) {
            if (!send_receiver_report(now)) {
                return;
            }
            next_report = now + config.report_interval;
        }
        if (!send_due_nacks(now)) {
            return;
        }
        if (!send_pending_pli()) {
            return;
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
            std::max(Clock::duration::zero(), next_report - Clock::now()));
        const auto timeout = std::min(config.receive_poll_interval, remaining);
        if (!receive_once(timeout)) {
            return;
        }
    }
}

bool DefaultReceiverRtcpWorker::Impl::send_pending_pli() {
    std::optional<std::uint32_t> source_ssrc;
    {
        std::lock_guard lock{mutex};
        source_ssrc = std::exchange(pending_pli_source_ssrc, std::nullopt);
    }
    if (!source_ssrc) {
        return true;
    }
    try {
        const common::rtcp::CompoundPacket compound{{
            common::rtcp::PictureLossIndication{local_ssrc, *source_ssrc}}};
        auto serialized = common::rtcp::serialize_compound_packet(compound);
        if (!serialized) {
            fail(worker_issue(ReceiverRtcpWorkerOperation::Serialize,
                              std::move(serialized.error())));
            return false;
        }
        if (auto sent = transport->send(*serialized); !sent) {
            fail(transport_issue(ReceiverRtcpWorkerOperation::Send,
                                 std::move(sent.error())));
            return false;
        }
        std::lock_guard lock{mutex};
        ++worker_stats.pli_packets_sent;
        return true;
    } catch (const std::exception& error) {
        fail(worker_issue(ReceiverRtcpWorkerOperation::Internal,
                          error.what()));
        return false;
    } catch (...) {
        fail(worker_issue(ReceiverRtcpWorkerOperation::Internal,
                          "unknown error while sending PLI"));
        return false;
    }
}

bool DefaultReceiverRtcpWorker::Impl::send_due_nacks(
    const Clock::time_point now) {
    try {
        auto batch = missing_tracker->take_due_nack_batch(now);
        if (!batch) {
            return true;
        }
        auto feedback =
            common::rtcp::pack_generic_nack_blocks(batch->sequences);
        common::rtcp::CompoundPacket compound{{common::rtcp::GenericNack{
            local_ssrc, batch->source_ssrc, std::move(feedback)}}};
        auto serialized = common::rtcp::serialize_compound_packet(compound);
        if (!serialized) {
            fail(worker_issue(ReceiverRtcpWorkerOperation::Serialize,
                              std::move(serialized.error())));
            return false;
        }
        if (auto sent = transport->send(*serialized); !sent) {
            fail(transport_issue(ReceiverRtcpWorkerOperation::Send,
                                 std::move(sent.error())));
            return false;
        }
        std::lock_guard lock{mutex};
        ++worker_stats.generic_nack_packets_sent;
        worker_stats.nack_sequence_requests_sent += batch->sequences.size();
        return true;
    } catch (const std::exception& error) {
        fail(worker_issue(ReceiverRtcpWorkerOperation::Internal,
                          error.what()));
        return false;
    } catch (...) {
        fail(worker_issue(ReceiverRtcpWorkerOperation::Internal,
                          "unknown error while sending Generic NACK"));
        return false;
    }
}

bool DefaultReceiverRtcpWorker::Impl::send_receiver_report(
    const Clock::time_point now) {
    auto block = reception_statistics->take_report();
    if (!block) {
        return true;
    }
    if (last_sr && last_sr_received_at) {
        auto delay = common::rtcp::compact_ntp_duration(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::max(Clock::duration::zero(),
                         now - *last_sr_received_at)));
        if (delay) {
            block->last_sender_report = *last_sr;
            block->delay_since_last_sender_report = *delay;
        }
    }

    common::rtcp::CompoundPacket compound{{
        common::rtcp::ReceiverReport{local_ssrc, {*block}},
        common::rtcp::SourceDescription{{
            {local_ssrc, canonical_name(local_ssrc)},
        }},
    }};
    auto serialized = common::rtcp::serialize_compound_packet(compound);
    if (!serialized) {
        fail(worker_issue(ReceiverRtcpWorkerOperation::Serialize,
                          std::move(serialized.error())));
        return false;
    }
    if (auto sent = transport->send(*serialized); !sent) {
        fail(transport_issue(ReceiverRtcpWorkerOperation::Send,
                             std::move(sent.error())));
        return false;
    }
    std::lock_guard lock{mutex};
    ++worker_stats.receiver_reports_sent;
    worker_stats.current_fraction_lost = block->fraction_lost;
    worker_stats.cumulative_lost = block->cumulative_lost;
    worker_stats.extended_highest_sequence =
        block->extended_highest_sequence;
    worker_stats.interarrival_jitter = block->interarrival_jitter;
    worker_stats.last_sender_report = block->last_sender_report;
    worker_stats.delay_since_last_sender_report =
        block->delay_since_last_sender_report;
    return true;
}

bool DefaultReceiverRtcpWorker::Impl::receive_once(
    const std::chrono::milliseconds timeout) {
    auto received = transport->receive_for(timeout);
    if (!received) {
        fail(transport_issue(ReceiverRtcpWorkerOperation::Receive,
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
        const auto* report = std::get_if<common::rtcp::SenderReport>(&packet);
        if (report != nullptr) {
            process_sender_report(*report, (*received)->received_at);
        }
    }
    return true;
}

void DefaultReceiverRtcpWorker::Impl::process_sender_report(
    const common::rtcp::SenderReport& report,
    const Clock::time_point received_at) {
    const auto reception = reception_statistics->stats();
    std::lock_guard lock{mutex};
    ++worker_stats.sender_reports_received;
    if (!reception.source_ssrc || report.sender_ssrc != *reception.source_ssrc) {
        ++worker_stats.ignored_sender_reports;
        return;
    }
    last_sr = common::rtcp::compact_ntp(report.ntp_timestamp);
    last_sr_received_at = received_at;
}

void DefaultReceiverRtcpWorker::Impl::fail(
    ReceiverRtcpWorkerIssue issue) noexcept {
    std::lock_guard lock{mutex};
    worker_stats.state = ReceiverRtcpWorkerState::Failed;
    worker_stats.last_issue = std::move(issue);
}

DefaultReceiverRtcpWorker::DefaultReceiverRtcpWorker(
    ReceiverRtcpWorkerConfig config,
    std::unique_ptr<common::rtcp::Transport> transport,
    std::shared_ptr<RtpReceptionStatistics> reception_statistics,
    std::shared_ptr<RtpMissingTracker> missing_tracker)
    : impl_{std::make_unique<Impl>(
          std::move(config), std::move(transport),
          std::move(reception_statistics), std::move(missing_tracker))} {}

DefaultReceiverRtcpWorker::~DefaultReceiverRtcpWorker() = default;

ReceiverRtcpStartResult DefaultReceiverRtcpWorker::start() {
    return impl_->start();
}

bool DefaultReceiverRtcpWorker::request_pli(
    const std::uint32_t media_source_ssrc) noexcept {
    return impl_->request_pli(media_source_ssrc);
}

void DefaultReceiverRtcpWorker::stop() noexcept {
    impl_->stop();
}

ReceiverRtcpWorkerState DefaultReceiverRtcpWorker::state() const noexcept {
    return impl_->state();
}

ReceiverRtcpWorkerStats DefaultReceiverRtcpWorker::stats() const noexcept {
    return impl_->stats();
}

}  // namespace semilive::receiver::domain
