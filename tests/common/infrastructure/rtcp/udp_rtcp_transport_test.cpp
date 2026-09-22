#include <semilive/common/rtcp/udp_rtcp_transport.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace rtcp = semilive::common::rtcp;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

void sends_and_receives_from_the_bound_endpoint() {
    rtcp::UdpTransport receiver;
    auto receiver_opened = receiver.open({
        .bind_address = "127.0.0.1",
        .bind_port = 0,
        .peer_address = "127.0.0.1",
        .peer_port = 9,
    });
    require(receiver_opened.has_value() && receiver_opened->bound_port != 0,
            "RTCP receiver did not bind an ephemeral port");

    rtcp::UdpTransport sender;
    auto sender_opened = sender.open({
        .bind_address = "127.0.0.1",
        .bind_port = 0,
        .peer_address = "127.0.0.1",
        .peer_port = receiver_opened->bound_port,
    });
    require(sender_opened.has_value(), "RTCP sender did not open");

    const std::vector<std::byte> datagram{
        std::byte{0x80}, std::byte{0xc9}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
    };
    require(sender.send(datagram).has_value(),
            "RTCP UDP datagram was not sent");
    const auto received = receiver.receive_for(500ms);
    require(received.has_value() && received->has_value(),
            "RTCP UDP datagram was not received");
    require((*received)->bytes == datagram,
            "RTCP UDP transport changed datagram bytes");
    require(!receiver.receive_for(10ms)->has_value(),
            "RTCP UDP timeout fabricated a datagram");

    sender.close();
    receiver.close();
}

void validates_endpoint_configuration_and_state() {
    rtcp::UdpTransport transport;
    const auto invalid = transport.open({
        .bind_address = "127.0.0.1",
        .peer_address = "127.0.0.1",
        .peer_port = 0,
    });
    require(!invalid, "RTCP UDP transport accepted a zero peer port");
    require(!transport.receive_for(0ms),
            "closed RTCP UDP transport accepted receive");
}

}  // namespace

int main() {
    try {
        sends_and_receives_from_the_bound_endpoint();
        validates_endpoint_configuration_and_state();
        std::cout << "RTCP UDP transport tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "RTCP UDP transport test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
