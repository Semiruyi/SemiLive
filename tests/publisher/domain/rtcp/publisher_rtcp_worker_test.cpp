#include <semilive/common/rtcp/rtcp_packet.hpp>
#include <semilive/common/rtcp/ntp_time.hpp>
#include <semilive/publisher/domain/rtcp/default_publisher_rtcp_worker.hpp>
#include <semilive/publisher/domain/rtp/rtp_sender_state.hpp>

#include "publisher/support/notifier/synchronous_notifier.hpp"

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

void sends_sr_and_consumes_matching_rr() {
    auto sender_state = std::make_shared<domain::RtpSenderState>();
    sender_state->begin_session(0x1122'3344U);
    sender_state->record_sent_packet(
        10U, 90'000U, 1'000U, {}, std::chrono::steady_clock::now());
    auto notifier = std::make_shared<SynchronousNotifier>();
    auto transport = std::make_unique<FakeTransport>();
    auto* transport_view = transport.get();
    domain::PublisherRtcpWorkerConfig config;
    config.transport.peer_address = "127.0.0.1";
    config.transport.peer_port = 5007;
    config.report_interval = 10ms;
    config.receive_poll_interval = 2ms;
    domain::DefaultPublisherRtcpWorker worker{
        config, std::move(transport), sender_state, notifier};

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
        sender_state_is_session_scoped();
        std::cout << "publisher RTCP worker tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "publisher RTCP worker test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
