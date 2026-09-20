#include <semilive/relay/application/relay_session.hpp>

#include <string>
#include <utility>

namespace semilive::relay::application {
namespace {

std::string describe_issue(
    const infra::network::UdpRelaySocketIssue& issue) {
    std::string result = issue.message;
    if (issue.native_code != 0) {
        result += " (native code " + std::to_string(issue.native_code) + ')';
    }
    return result;
}

}  // namespace

RelaySession::RelaySession(RelayConfig config)
    : config_{std::move(config)},
      loss_policy_{config_.loss_rate_ppm, config_.random_seed} {}

RelaySessionOpenResult RelaySession::open() {
    if (started_) {
        return std::unexpected{
            "relay session can only be opened once"};
    }
    if (config_.poll_interval <= std::chrono::milliseconds::zero()) {
        return std::unexpected{
            "relay poll interval must be greater than zero"};
    }
    started_ = true;

    auto opened = socket_.open(config_.network);
    if (!opened) {
        return std::unexpected{describe_issue(opened.error())};
    }
    open_ = true;
    return RelaySessionInfo{opened->bound_address, opened->bound_port,
                            opened->maximum_datagram_bytes,
                            opened->receive_buffer_bytes};
}

RelaySessionPollResult RelaySession::poll_once() {
    if (!open_) {
        return std::unexpected{
            "relay session must be open before polling"};
    }

    auto received = socket_.receive_for(config_.poll_interval);
    if (!received) {
        return std::unexpected{describe_issue(received.error())};
    }
    if (!*received) {
        ++stats_.receive_timeouts;
        return RelayPollStatus::Timeout;
    }

    const auto& datagram = **received;
    ++stats_.received_datagrams;
    stats_.received_bytes += datagram.size();
    if (loss_policy_.should_drop()) {
        ++stats_.dropped_datagrams;
        stats_.dropped_bytes += datagram.size();
        return RelayPollStatus::Dropped;
    }

    if (auto sent = socket_.send(datagram); !sent) {
        ++stats_.send_failures;
        return std::unexpected{describe_issue(sent.error())};
    }
    ++stats_.forwarded_datagrams;
    stats_.forwarded_bytes += datagram.size();
    return RelayPollStatus::Forwarded;
}

void RelaySession::close() noexcept {
    socket_.close();
    open_ = false;
}

const RelayConfig& RelaySession::config() const noexcept {
    return config_;
}

const RelaySessionStats& RelaySession::stats() const noexcept {
    return stats_;
}

}  // namespace semilive::relay::application
