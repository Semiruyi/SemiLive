#include <semilive/common/rtcp/rtcp_packet.hpp>
#include <semilive/common/rtcp/ntp_time.hpp>
#include <semilive/receiver/domain/rtcp/default_receiver_rtcp_worker.hpp>
#include <semilive/receiver/domain/rtp/rtp_reception_statistics.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
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
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace common_rtcp = semilive::common::rtcp;
namespace domain = semilive::receiver::domain;

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
        return common_rtcp::TransportInfo{"127.0.0.1", 5007, 1500, 262144};
    }

    common_rtcp::TransportReceiveResult receive_for(
        const std::chrono::milliseconds timeout) override {
        std::unique_lock lock{mutex_};
        cv_.wait_for(lock, timeout,
                     [this] { return !incoming_.empty() || !opened_; });
        if (!opened_ || incoming_.empty()) {
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

    [[nodiscard]] std::vector<std::byte> wait_for_sent(
        const std::size_t count) {
        std::unique_lock lock{mutex_};
        require(cv_.wait_for(lock, 500ms,
                             [this, count] { return sent_.size() >= count; }),
                "receiver RTCP worker did not send enough reports");
        return sent_[count - 1U];
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

[[nodiscard]] const common_rtcp::ReceiverReport& receiver_report(
    const common_rtcp::CompoundPacket& packet) {
    require(!packet.packets.empty(), "RTCP compound packet is empty");
    const auto* report =
        std::get_if<common_rtcp::ReceiverReport>(&packet.packets.front());
    require(report != nullptr, "RTCP compound packet does not start with RR");
    return *report;
}

void sends_rr_and_tracks_the_last_sender_report() {
    auto reception = std::make_shared<domain::RtpReceptionStatistics>();
    const auto started = std::chrono::steady_clock::now();
    reception->observe(0x1122'3344U, 10U, 0U, started);
    reception->observe(0x1122'3344U, 12U, 6'000U, started + 66ms);

    auto transport = std::make_unique<FakeTransport>();
    auto* transport_view = transport.get();
    domain::ReceiverRtcpWorkerConfig config;
    config.transport.peer_address = "127.0.0.1";
    config.transport.peer_port = 5005;
    config.report_interval = 10ms;
    config.receive_poll_interval = 2ms;
    config.local_ssrc = 0xaabb'ccddU;
    domain::DefaultReceiverRtcpWorker worker{
        config, std::move(transport), reception};

    require(worker.start().has_value(), "receiver RTCP worker did not start");
    const auto first_bytes = transport_view->wait_for_sent(1U);
    const auto first = common_rtcp::parse_compound_packet(first_bytes);
    require(first.has_value() && first->packets.size() == 2,
            "receiver RTCP worker did not send RR plus SDES");
    const auto& first_rr = receiver_report(*first);
    require(first_rr.sender_ssrc == 0xaabb'ccddU &&
                first_rr.reports.size() == 1U &&
                first_rr.reports.front().source_ssrc == 0x1122'3344U &&
                first_rr.reports.front().cumulative_lost == 1,
            "receiver report did not use RTP reception statistics");

    const common_rtcp::SenderReport sender_report{
        0x1122'3344U,
        0x1234'5678'9abc'def0ULL,
        9'000U,
        3U,
        2'000U,
        {},
    };
    auto encoded_sender_report =
        common_rtcp::serialize_compound_packet({{sender_report}});
    require(encoded_sender_report.has_value(),
            "test sender report did not serialize");
    transport_view->inject(std::move(*encoded_sender_report));

    const auto second_bytes = transport_view->wait_for_sent(2U);
    const auto second = common_rtcp::parse_compound_packet(second_bytes);
    require(second.has_value(), "second receiver report did not parse");
    const auto& second_block = receiver_report(*second).reports.front();
    require(second_block.last_sender_report ==
                common_rtcp::compact_ntp(sender_report.ntp_timestamp),
            "receiver report did not reference the last sender report");
    const auto stats = worker.stats();
    require(stats.sender_reports_received == 1U &&
                stats.receiver_reports_sent >= 2U,
            "receiver RTCP worker statistics are incomplete");
    worker.stop();
    require(worker.state() == domain::ReceiverRtcpWorkerState::Idle,
            "receiver RTCP worker did not return to idle");
}

}  // namespace

int main() {
    try {
        sends_rr_and_tracks_the_last_sender_report();
        std::cout << "receiver RTCP worker tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "receiver RTCP worker test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
