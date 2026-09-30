#include <semilive/common/rtcp/rtcp_packet.hpp>
#include <semilive/common/rtcp/ntp_time.hpp>
#include <semilive/publisher/domain/rtcp/default_publisher_rtcp_worker.hpp>
#include <semilive/publisher/domain/rtcp/publisher_rtcp_worker_events.hpp>
#include <semilive/publisher/domain/rtp/rtp_sender_state.hpp>
#include <semilive/publisher/contracts/output/rtp_retransmission_sender.hpp>

#include "publisher/support/notifier/synchronous_notifier.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace common_rtcp = semilive::common::rtcp;
namespace domain = semilive::publisher::domain;
using semilive::publisher::test_support::SynchronousNotifier;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

class FakeTransport final : public common_rtcp::Transport {
public:
    common_rtcp::TransportOpenResult open(
        const common_rtcp::TransportConfig&) override {
        std::lock_guard lock{mutex_};
        opened_ = true;
        return common_rtcp::TransportInfo{"127.0.0.1", 5005, 1500, 262144};
    }

    common_rtcp::TransportReceiveResult receive_for(
        const std::chrono::milliseconds timeout) override {
        std::unique_lock lock{mutex_};
        cv_.wait_for(lock, timeout,
                     [this] { return !incoming_.empty() || !opened_; });
        if (!opened_) {
            return std::optional<common_rtcp::ReceivedDatagram>{};
        }
        if (incoming_.empty()) {
            return std::optional<common_rtcp::ReceivedDatagram>{};
        }
        auto datagram = std::move(incoming_.front());
        incoming_.pop_front();
        return std::optional<common_rtcp::ReceivedDatagram>{
            std::move(datagram)};
    }

    common_rtcp::TransportSendResult send(
        const std::span<const std::byte> datagram) override {
        std::lock_guard lock{mutex_};
        sent_.emplace_back(datagram.begin(), datagram.end());
        cv_.notify_all();
        return {};
    }

    void close() noexcept override {
        std::lock_guard lock{mutex_};
        opened_ = false;
        cv_.notify_all();
    }

    [[nodiscard]] std::vector<std::byte> wait_for_sent() {
        std::unique_lock lock{mutex_};
        require(cv_.wait_for(lock, 500ms, [this] { return !sent_.empty(); }),
                "publisher RTCP worker did not send a report");
        return sent_.front();
    }

    void inject(std::vector<std::byte> bytes) {
        std::lock_guard lock{mutex_};
        incoming_.push_back({std::move(bytes),
                             std::chrono::steady_clock::now()});
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool opened_ = false;
    std::deque<common_rtcp::ReceivedDatagram> incoming_;
    std::vector<std::vector<std::byte>> sent_;
};

class FakeRetransmissionSender final
    : public semilive::publisher::contracts::output::RtpRetransmissionSender {
public:
    semilive::publisher::contracts::output::RtpRetransmissionSenderResult
    open() override {
        std::lock_guard lock{mutex_};
        opened_ = true;
        return {};
    }

    semilive::publisher::contracts::output::RtpRetransmissionSenderResult
    send(const std::span<const std::byte> datagram) override {
        std::lock_guard lock{mutex_};
        if (!opened_) {
            return std::unexpected{
                semilive::publisher::contracts::output::
                    RtpRetransmissionSenderIssue{
                        0, "fake retransmission sender is closed"}};
        }
        sent_.emplace_back(datagram.begin(), datagram.end());
        return {};
    }

    void close() noexcept override {
        std::lock_guard lock{mutex_};
        opened_ = false;
    }

    [[nodiscard]] std::vector<std::vector<std::byte>> sent() const {
        std::lock_guard lock{mutex_};
        return sent_;
    }

private:
    mutable std::mutex mutex_;
    bool opened_ = false;
    std::vector<std::vector<std::byte>> sent_;
};

void sends_sr_and_consumes_matching_rr() {
    auto sender_state = std::make_shared<domain::RtpSenderState>();
    sender_state->begin_session(0x1122'3344U);
    sender_state->record_sent_packet(
        10U, 90'000U, 1'000U, {}, std::chrono::steady_clock::now());
    auto notifier = std::make_shared<SynchronousNotifier>();
    auto transport = std::make_unique<FakeTransport>();
    auto* transport_view = transport.get();
    auto retransmission_sender =
        std::make_unique<FakeRetransmissionSender>();
    domain::PublisherRtcpWorkerConfig config;
    config.transport.peer_address = "127.0.0.1";
    config.transport.peer_port = 5007;
    config.report_interval = 10ms;
    config.receive_poll_interval = 2ms;
    domain::DefaultPublisherRtcpWorker worker{
        config, std::move(transport), sender_state,
        std::move(retransmission_sender), notifier};

    require(worker.start().has_value(), "publisher RTCP worker did not start");
    const auto sent = transport_view->wait_for_sent();
    const auto parsed = common_rtcp::parse_compound_packet(sent);
    require(parsed.has_value() && parsed->packets.size() == 2,
            "publisher RTCP worker did not send SR plus SDES");
    const auto* report =
        std::get_if<common_rtcp::SenderReport>(&parsed->packets.front());
    require(report != nullptr && report->sender_ssrc == 0x1122'3344U &&
                report->sender_packet_count == 1U &&
                report->sender_octet_count == 1'000U,
            "publisher sender report did not use RTP sender state");

    const common_rtcp::ReceiverReport response{
        0xaabb'ccddU,
        {{
            .source_ssrc = report->sender_ssrc,
            .fraction_lost = 32U,
            .cumulative_lost = 2,
            .extended_highest_sequence = 200U,
            .interarrival_jitter = 45U,
            .last_sender_report = common_rtcp::compact_ntp(
                report->ntp_timestamp),
        }},
    };
    auto encoded_response = common_rtcp::serialize_compound_packet({{response}});
    require(encoded_response.has_value(), "test receiver report did not serialize");
    transport_view->inject(std::move(*encoded_response));

    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (worker.stats().receiver_reports_received == 0U &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const auto stats = worker.stats();
    require(stats.receiver_reports_received == 1U &&
                stats.reported_fraction_lost == 32U &&
                stats.reported_cumulative_lost == 2 &&
                stats.reported_jitter == 45U && stats.rtt_samples == 1U &&
                stats.current_rtt.has_value(),
            "publisher RTCP worker did not consume the receiver report");
    worker.stop();
    require(worker.state() == domain::PublisherRtcpWorkerState::Idle,
            "publisher RTCP worker did not return to idle");
}

void retransmits_cached_rtp_for_matching_generic_nacks() {
    auto sender_state = std::make_shared<domain::RtpSenderState>();
    sender_state->begin_session(0x1122'3344U);
    const std::vector<std::byte> cached{
        std::byte{0x80}, std::byte{0x60}, std::byte{0x00}, std::byte{0x0a},
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
    };
    sender_state->record_sent_packet(
        10U, 90'000U, 0U, cached, std::chrono::steady_clock::now());

    auto notifier = std::make_shared<SynchronousNotifier>();
    auto transport = std::make_unique<FakeTransport>();
    auto* transport_view = transport.get();
    auto retransmission_sender =
        std::make_unique<FakeRetransmissionSender>();
    auto* retransmission_view = retransmission_sender.get();
    domain::PublisherRtcpWorkerConfig config;
    config.transport.peer_address = "127.0.0.1";
    config.transport.peer_port = 5007;
    config.report_interval = 1s;
    config.receive_poll_interval = 2ms;
    config.maximum_nack_sequence_requests = 2U;
    domain::DefaultPublisherRtcpWorker worker{
        config, std::move(transport), sender_state,
        std::move(retransmission_sender), notifier};

    require(worker.start().has_value(),
            "publisher RTCP worker did not start for NACK test");
    const common_rtcp::GenericNack nack{
        0xaabb'ccddU,
        0x1122'3344U,
        {{10U, 0x0001U}, {10U, 0x0003U}},
    };
    auto encoded = common_rtcp::serialize_compound_packet({{nack}});
    require(encoded.has_value(), "test Generic NACK did not serialize");
    transport_view->inject(std::move(*encoded));

    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (worker.stats().retransmission_cache_misses == 0U &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    auto stats = worker.stats();
    require(stats.generic_nack_packets_received == 1U &&
                stats.nack_sequence_requests_received == 2U &&
                stats.nack_sequence_requests_ignored == 3U &&
                stats.retransmission_cache_hits == 1U &&
                stats.retransmission_cache_misses == 1U &&
                stats.retransmitted_packets == 1U &&
                stats.retransmitted_bytes == cached.size() &&
                retransmission_view->sent() ==
                    std::vector<std::vector<std::byte>>{cached},
            "publisher did not retransmit the exact cached RTP datagram");

    const common_rtcp::GenericNack wrong_source{
        0xaabb'ccddU,
        0x5566'7788U,
        common_rtcp::pack_generic_nack_blocks(
            std::vector<std::uint16_t>{10U}),
    };
    auto encoded_wrong_source =
        common_rtcp::serialize_compound_packet({{wrong_source}});
    require(encoded_wrong_source.has_value(),
            "wrong-source Generic NACK did not serialize");
    transport_view->inject(std::move(*encoded_wrong_source));
    const auto wrong_source_deadline =
        std::chrono::steady_clock::now() + 500ms;
    while (worker.stats().ignored_generic_nack_packets == 0U &&
           std::chrono::steady_clock::now() < wrong_source_deadline) {
        std::this_thread::yield();
    }
    stats = worker.stats();
    require(stats.ignored_generic_nack_packets == 1U &&
                stats.retransmitted_packets == 1U,
            "publisher accepted a Generic NACK for another RTP source");
    worker.stop();
}

void reports_matching_picture_loss_indications_once() {
    auto sender_state = std::make_shared<domain::RtpSenderState>();
    sender_state->begin_session(0x1122'3344U);
    auto notifier = std::make_shared<SynchronousNotifier>();
    std::atomic<unsigned> events{0U};
    auto subscription = notifier->subscribe<
        domain::PublisherPictureLossIndicationReceived>(
        [&events](const domain::PublisherPictureLossIndicationReceived& event) {
            if (event.sender_ssrc == 0xaabb'ccddU &&
                event.media_source_ssrc == 0x1122'3344U) {
                events.fetch_add(1U);
            }
        });
    auto transport = std::make_unique<FakeTransport>();
    auto* transport_view = transport.get();
    domain::PublisherRtcpWorkerConfig config;
    config.transport.peer_address = "127.0.0.1";
    config.transport.peer_port = 5007;
    config.report_interval = 1s;
    config.receive_poll_interval = 2ms;
    domain::DefaultPublisherRtcpWorker worker{
        config, std::move(transport), sender_state,
        std::make_unique<FakeRetransmissionSender>(), notifier};
    require(worker.start().has_value(),
            "publisher RTCP worker did not start for PLI test");

    const auto valid = common_rtcp::serialize_compound_packet(
        {{common_rtcp::PictureLossIndication{0xaabb'ccddU,
                                             0x1122'3344U}}});
    const auto wrong = common_rtcp::serialize_compound_packet(
        {{common_rtcp::PictureLossIndication{0xaabb'ccddU,
                                             0x5566'7788U}}});
    require(valid.has_value() && wrong.has_value(),
            "test PLI did not serialize");
    transport_view->inject(*valid);
    transport_view->inject(*wrong);
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (worker.stats().pli_packets_received < 2U &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    worker.stop();
    const auto stats = worker.stats();
    require(stats.pli_packets_received == 2U &&
                stats.ignored_pli_packets == 1U &&
                events.load() == 1U,
            "publisher did not filter and report PLI exactly once");
    static_cast<void>(subscription->unsubscribe());
}

void sender_state_is_session_scoped() {
    domain::RtpSenderState state;
    state.begin_session(9U);
    const auto datagram = std::vector<std::byte>{std::byte{1}, std::byte{2}};
    state.record_sent_packet(10U, 100U, 7U, datagram,
                             std::chrono::steady_clock::time_point{});
    const auto snapshot = state.snapshot();
    require(snapshot && snapshot->ssrc == 9U &&
                snapshot->packet_count == 1U &&
                snapshot->payload_octet_count == 7U,
            "RTP sender state did not record a packet");
    require(state.find_retransmission_packet(
                10U, std::chrono::steady_clock::time_point{}) == datagram,
            "RTP sender state did not cache the sent datagram");
    state.end_session();
    require(!state.snapshot() &&
                !state.find_retransmission_packet(
                    10U, std::chrono::steady_clock::time_point{}),
            "RTP sender state survived session end");
}

}  // namespace

int main() {
    try {
        sends_sr_and_consumes_matching_rr();
        retransmits_cached_rtp_for_matching_generic_nacks();
        reports_matching_picture_loss_indications_once();
        sender_state_is_session_scoped();
        std::cout << "publisher RTCP worker tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "publisher RTCP worker test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
