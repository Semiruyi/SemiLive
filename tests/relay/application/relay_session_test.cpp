#include <semilive/relay/application/relay_session.hpp>
#include <semilive/relay/infrastructure/network/udp_relay_socket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace application = semilive::relay::application;
namespace domain = semilive::relay::domain;
namespace network = semilive::relay::infra::network;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

network::UdpRelaySocketConfig socket_config(
    const std::uint16_t forward_port) {
    network::UdpRelaySocketConfig config;
    config.bind_port = 0;
    config.forward_port = forward_port;
    return config;
}

void forwards_without_loss() {
    network::UdpRelaySocket destination;
    const auto destination_info = destination.open(socket_config(9));
    require(destination_info.has_value(), "destination must open");

    application::RelayConfig config;
    config.network.bind_port = 0;
    config.network.forward_port = destination_info->bound_port;
    config.poll_interval = 100ms;
    application::RelaySession relay{config};
    const auto relay_info = relay.open();
    require(relay_info.has_value(), "relay session must open");

    network::UdpRelaySocket source;
    require(source.open(socket_config(relay_info->bound_port)).has_value(),
            "source must open");
    const std::vector payload{std::byte{0x01}, std::byte{0x02},
                              std::byte{0x03}};
    require(source.send(payload).has_value(), "source must send data");
    const auto poll = relay.poll_once();
    require(poll && *poll == application::RelayPollStatus::Forwarded,
            "zero-loss relay must forward the datagram");
    const auto received = destination.receive_for(500ms);
    require(received && received->has_value() && **received == payload,
            "relay must preserve the forwarded payload");

    const auto& stats = relay.stats();
    require(stats.received_datagrams == 1 &&
                stats.forwarded_datagrams == 1 &&
                stats.dropped_datagrams == 0 &&
                stats.received_bytes == payload.size() &&
                stats.forwarded_bytes == payload.size(),
            "relay must count a forwarded datagram and its bytes");
}

void drops_at_full_loss() {
    network::UdpRelaySocket destination;
    const auto destination_info = destination.open(socket_config(9));
    require(destination_info.has_value(), "destination must open");

    application::RelayConfig config;
    config.network.bind_port = 0;
    config.network.forward_port = destination_info->bound_port;
    config.loss_rate_ppm = domain::RandomLossPolicy::rate_scale;
    config.poll_interval = 100ms;
    application::RelaySession relay{config};
    const auto relay_info = relay.open();
    require(relay_info.has_value(), "lossy relay session must open");

    network::UdpRelaySocket source;
    require(source.open(socket_config(relay_info->bound_port)).has_value(),
            "source must open");
    const std::vector payload{std::byte{0xaa}, std::byte{0xbb}};
    require(source.send(payload).has_value(), "source must send data");
    const auto poll = relay.poll_once();
    require(poll && *poll == application::RelayPollStatus::Dropped,
            "full-loss relay must drop the datagram");
    const auto received = destination.receive_for(20ms);
    require(received && !received->has_value(),
            "dropped datagram must not reach the destination");

    const auto& stats = relay.stats();
    require(stats.received_datagrams == 1 &&
                stats.forwarded_datagrams == 0 &&
                stats.dropped_datagrams == 1 &&
                stats.dropped_bytes == payload.size(),
            "relay must count a dropped datagram and its bytes");
}

}  // namespace

int main() {
    try {
        forwards_without_loss();
        drops_at_full_loss();
    } catch (const std::exception& error) {
        std::cerr << "relay session test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
