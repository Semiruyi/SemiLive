#pragma once

#include <semilive/relay/domain/random_loss_policy.hpp>
#include <semilive/relay/infrastructure/network/udp_relay_socket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace semilive::relay::application {

struct RelayConfig {
    infra::network::UdpRelaySocketConfig network;
    std::uint32_t loss_rate_ppm = 0;
    std::uint64_t random_seed = 1;
    std::chrono::milliseconds poll_interval{10};
};

struct RelaySessionInfo {
    std::string bound_address;
    std::uint16_t bound_port = 0;
    std::size_t maximum_datagram_bytes = 0;
    std::size_t receive_buffer_bytes = 0;
};

struct RelaySessionStats {
    std::uint64_t received_datagrams = 0;
    std::uint64_t received_bytes = 0;
    std::uint64_t forwarded_datagrams = 0;
    std::uint64_t forwarded_bytes = 0;
    std::uint64_t dropped_datagrams = 0;
    std::uint64_t dropped_bytes = 0;
    std::uint64_t receive_timeouts = 0;
    std::uint64_t send_failures = 0;
};

enum class RelayPollStatus {
    Timeout,
    Forwarded,
    Dropped,
};

using RelaySessionOpenResult =
    std::expected<RelaySessionInfo, std::string>;
using RelaySessionPollResult =
    std::expected<RelayPollStatus, std::string>;

class RelaySession final {
public:
    explicit RelaySession(RelayConfig config);

    RelaySession(const RelaySession&) = delete;
    RelaySession& operator=(const RelaySession&) = delete;
    RelaySession(RelaySession&&) = delete;
    RelaySession& operator=(RelaySession&&) = delete;

    [[nodiscard]] RelaySessionOpenResult open();
    [[nodiscard]] RelaySessionPollResult poll_once();
    void close() noexcept;

    [[nodiscard]] const RelayConfig& config() const noexcept;
    [[nodiscard]] const RelaySessionStats& stats() const noexcept;

private:
    RelayConfig config_;
    domain::RandomLossPolicy loss_policy_;
    infra::network::UdpRelaySocket socket_;
    RelaySessionStats stats_;
    bool started_ = false;
    bool open_ = false;
};

}  // namespace semilive::relay::application
