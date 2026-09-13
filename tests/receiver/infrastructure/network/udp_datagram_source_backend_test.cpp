#include <semilive/receiver/infrastructure/network/udp_datagram_source_backend.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

namespace contract = semilive::receiver::contracts::network;
namespace infra = semilive::receiver::infra::network;
namespace model = semilive::receiver::model;

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket invalid_socket = -1;
#endif

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

class SocketRuntime final {
public:
    SocketRuntime() {
#if defined(_WIN32)
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0,
                "test failed to initialize Winsock");
#endif
    }

    ~SocketRuntime() {
#if defined(_WIN32)
        static_cast<void>(WSACleanup());
#endif
    }

    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
};

void close_socket(const NativeSocket socket) noexcept {
    if (socket == invalid_socket) {
        return;
    }
#if defined(_WIN32)
    static_cast<void>(closesocket(socket));
#else
    static_cast<void>(::close(socket));
#endif
}

void send_loopback(const std::uint16_t port,
                   const std::span<const std::byte> payload) {
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    require(socket != invalid_socket, "test failed to create UDP sender");

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr) == 1,
            "test failed to parse loopback address");
#if defined(_WIN32)
    const auto sent = ::sendto(
        socket, reinterpret_cast<const char*>(payload.data()),
        static_cast<int>(payload.size()), 0,
        reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
    const bool complete = sent != SOCKET_ERROR &&
                          static_cast<std::size_t>(sent) == payload.size();
#else
    const auto sent = ::sendto(
        socket, payload.data(), payload.size(), 0,
        reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
    const bool complete = sent != -1 &&
                          static_cast<std::size_t>(sent) == payload.size();
#endif
    close_socket(socket);
    require(complete, "test failed to send complete UDP datagram");
}

[[nodiscard]] const model::UdpDatagram& received_datagram(
    const contract::DatagramSourceReceiveResult& result,
    const std::string_view message) {
    require(result.has_value(), message);
    const auto* datagram = std::get_if<model::UdpDatagram>(&*result);
    require(datagram != nullptr, message);
    return *datagram;
}

void receives_owned_datagrams_over_ipv4_loopback() {
    infra::UdpDatagramSourceBackend source;
    const auto opened = source.open({"127.0.0.1", 0, 65'507});
    require(opened && opened->bound_address == "127.0.0.1" &&
                opened->bound_port != 0 &&
                opened->maximum_datagram_bytes == 65'507,
            "source must report its effective ephemeral endpoint");

    const std::vector payload{
        std::byte{0x80}, std::byte{0x60}, std::byte{0xde}, std::byte{0xad}};
    const auto sent_before = model::UdpDatagram::Clock::now();
    send_loopback(opened->bound_port, payload);
    const auto result = source.receive_for(500ms);
    const auto& datagram = received_datagram(
        result, "source must receive one loopback datagram");
    require(datagram.bytes == payload,
            "source must preserve exact UDP payload bytes");
    require(datagram.received_at >= sent_before &&
                datagram.received_at <= model::UdpDatagram::Clock::now(),
            "source must stamp data with a monotonic receive time");
}

void returns_timeout_and_discards_datagrams_above_the_configured_limit() {
    infra::UdpDatagramSourceBackend source;
    const auto opened = source.open({"127.0.0.1", 0, 4});
    require(opened.has_value(), "bounded source must open");

    const std::vector oversized(5, std::byte{0xaa});
    send_loopback(opened->bound_port, oversized);
    const auto timeout = source.receive_for(30ms);
    require(timeout &&
                std::holds_alternative<contract::DatagramReceiveTimeout>(
                    *timeout),
            "oversized datagram must be discarded until the read deadline");

    const std::vector accepted(4, std::byte{0xbb});
    send_loopback(opened->bound_port, accepted);
    require(received_datagram(source.receive_for(500ms),
                              "limit-sized datagram must be received")
                    .bytes == accepted,
            "discarding oversized traffic must not poison later reads");
}

void enforces_configuration_and_reusable_lifecycle() {
    infra::UdpDatagramSourceBackend source;
    const auto closed_receive = source.receive_for(0ms);
    require(!closed_receive &&
                closed_receive.error().operation ==
                    contract::DatagramSourceOperation::State,
            "receive while closed must report a state issue");
    require(!source.open({"", 5004, 1200}),
            "empty bind address must be rejected");
    require(!source.open({"localhost", 5004, 1200}),
            "hostnames must not trigger DNS resolution");
    require(!source.open({"127.0.0.1", 5004, 0}),
            "zero datagram bound must be rejected");
    require(!source.open({"127.0.0.1", 5004, 65'508}),
            "datagram bound above UDP payload limit must be rejected");

    const auto first = source.open({"127.0.0.1", 0, 1200});
    require(first.has_value(), "valid source must open");
    const auto duplicate = source.open({"127.0.0.1", 0, 1200});
    require(!duplicate &&
                duplicate.error().operation ==
                    contract::DatagramSourceOperation::State,
            "already-open source must reject duplicate open");
    const auto negative_timeout = source.receive_for(-1ms);
    require(!negative_timeout &&
                negative_timeout.error().operation ==
                    contract::DatagramSourceOperation::Receive,
            "negative read timeout must be rejected");

    source.close();
    source.close();
    const auto reopened = source.open({"127.0.0.1", 0, 1200});
    require(reopened && reopened->bound_port != 0,
            "closed source must support a fresh session");
    source.close();
}

}  // namespace

int main() {
    try {
        const SocketRuntime runtime;
        receives_owned_datagrams_over_ipv4_loopback();
        returns_timeout_and_discards_datagrams_above_the_configured_limit();
        enforces_configuration_and_reusable_lifecycle();
    } catch (const std::exception& error) {
        std::cerr << "UDP datagram source test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
