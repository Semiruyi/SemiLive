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
namespace network = semilive::relay::infra::network;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

network::UdpRelaySocketConfig config_for(const std::uint16_t forward_port) {
    network::UdpRelaySocketConfig config;
    config.bind_port = 0;
    config.forward_port = forward_port;
    return config;
}

void preserves_udp_datagram_boundaries() {
    network::UdpRelaySocket destination;
    const auto destination_info = destination.open(config_for(9));
    require(destination_info && destination_info->bound_port != 0,
            "destination socket must bind an ephemeral port");

    network::UdpRelaySocket source;
    const auto source_info =
        source.open(config_for(destination_info->bound_port));
    require(source_info.has_value(), "source socket must open");

    const std::vector payload{std::byte{0x80}, std::byte{0x60},
                              std::byte{0xde}, std::byte{0xad}};
    require(source.send(payload).has_value(),
            "source must send the complete datagram");
    const auto received = destination.receive_for(500ms);
    require(received && received->has_value() && **received == payload,
            "destination must receive exactly one unchanged datagram");
    require(destination.receive_for(10ms)->has_value() == false,
            "receive must report a timeout without fabricating data");
}

void enforces_socket_lifecycle() {
    network::UdpRelaySocket socket;
    require(!socket.receive_for(0ms),
            "receiving while closed must fail");
    require(!socket.send({}), "sending while closed must fail");

    auto invalid = config_for(0);
    require(!socket.open(invalid), "zero forward port must be rejected");
    auto wildcard_loop = config_for(50'004);
    wildcard_loop.bind_address = "0.0.0.0";
    wildcard_loop.bind_port = 50'004;
    require(!socket.open(wildcard_loop),
            "wildcard bind must not forward back to its own port");
    auto valid = config_for(9);
    require(socket.open(valid).has_value(), "valid socket must open");
    require(!socket.open(valid), "an open socket must reject a second open");
    socket.close();
    socket.close();
    require(socket.open(valid).has_value(),
            "a closed socket must support a new session");
}

}  // namespace

int main() {
    try {
        preserves_udp_datagram_boundaries();
        enforces_socket_lifecycle();
    } catch (const std::exception& error) {
        std::cerr << "UDP relay socket test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
