#include "udp_socket.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace {

namespace detail = semilive::publisher::infra::output::detail;

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

void close_native_socket(const NativeSocket socket) noexcept {
    if (socket == invalid_socket) {
        return;
    }
#if defined(_WIN32)
    static_cast<void>(closesocket(socket));
#else
    static_cast<void>(::close(socket));
#endif
}

class LoopbackReceiver {
public:
    static std::optional<LoopbackReceiver> create(const int family) {
        LoopbackReceiver receiver;
#if defined(_WIN32)
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return std::nullopt;
        }
        receiver.winsock_started_ = true;
#endif
        receiver.socket_ = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
        if (receiver.socket_ == invalid_socket) {
            return std::nullopt;
        }

        if (family == AF_INET) {
            sockaddr_in endpoint{};
            endpoint.sin_family = AF_INET;
            endpoint.sin_port = 0;
            endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::bind(receiver.socket_,
                       reinterpret_cast<const sockaddr*>(&endpoint),
                       sizeof(endpoint)) != 0) {
                return std::nullopt;
            }
            socklen_t endpoint_size = sizeof(endpoint);
            if (getsockname(receiver.socket_,
                            reinterpret_cast<sockaddr*>(&endpoint),
                            &endpoint_size) != 0) {
                return std::nullopt;
            }
            receiver.port_ = ntohs(endpoint.sin_port);
            receiver.address_ = "127.0.0.1";
        } else if (family == AF_INET6) {
            sockaddr_in6 endpoint{};
            endpoint.sin6_family = AF_INET6;
            endpoint.sin6_port = 0;
            endpoint.sin6_addr = in6addr_loopback;
            if (::bind(receiver.socket_,
                       reinterpret_cast<const sockaddr*>(&endpoint),
                       sizeof(endpoint)) != 0) {
                return std::nullopt;
            }
            socklen_t endpoint_size = sizeof(endpoint);
            if (getsockname(receiver.socket_,
                            reinterpret_cast<sockaddr*>(&endpoint),
                            &endpoint_size) != 0) {
                return std::nullopt;
            }
            receiver.port_ = ntohs(endpoint.sin6_port);
            receiver.address_ = "::1";
        } else {
            return std::nullopt;
        }

#if defined(_WIN32)
        const DWORD timeout_milliseconds = 2000;
        if (setsockopt(receiver.socket_, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout_milliseconds),
                       sizeof(timeout_milliseconds)) != 0) {
            return std::nullopt;
        }
#else
        const timeval timeout{2, 0};
        if (setsockopt(receiver.socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) != 0) {
            return std::nullopt;
        }
#endif
        return receiver;
    }

    ~LoopbackReceiver() {
        reset();
    }

    LoopbackReceiver(const LoopbackReceiver&) = delete;
    LoopbackReceiver& operator=(const LoopbackReceiver&) = delete;

    LoopbackReceiver(LoopbackReceiver&& other) noexcept
        : socket_{std::exchange(other.socket_, invalid_socket)},
          port_{std::exchange(other.port_, 0)},
          address_{std::move(other.address_)}
#if defined(_WIN32)
          ,
          winsock_started_{std::exchange(other.winsock_started_, false)}
#endif
    {}

    LoopbackReceiver& operator=(LoopbackReceiver&&) = delete;

    [[nodiscard]] std::string_view address() const noexcept {
        return address_;
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    [[nodiscard]] std::vector<std::byte> receive() const {
        std::array<std::byte, 65'507> buffer{};
#if defined(_WIN32)
        const auto received = ::recv(
            socket_, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0);
        require(received != SOCKET_ERROR,
                "loopback receiver timed out or failed");
#else
        const auto received = ::recv(socket_, buffer.data(), buffer.size(), 0);
        require(received != -1, "loopback receiver timed out or failed");
#endif
        return {buffer.begin(), buffer.begin() + received};
    }

private:
    LoopbackReceiver() = default;

    void reset() noexcept {
        close_native_socket(socket_);
        socket_ = invalid_socket;
#if defined(_WIN32)
        if (winsock_started_) {
            static_cast<void>(WSACleanup());
            winsock_started_ = false;
        }
#endif
    }

    NativeSocket socket_ = invalid_socket;
    std::uint16_t port_ = 0;
    std::string address_;
#if defined(_WIN32)
    bool winsock_started_ = false;
#endif
};

void sends_complete_datagram_over_loopback(const int family) {
    auto receiver = LoopbackReceiver::create(family);
    if (!receiver) {
        if (family == AF_INET6) {
            std::cout << "IPv6 loopback unavailable; skipping IPv6 case\n";
            return;
        }
        throw std::runtime_error{"IPv4 loopback receiver is unavailable"};
    }

    detail::UdpSocket sender;
    const std::vector payload{
        std::byte{0x80}, std::byte{0x60}, std::byte{0x00}, std::byte{0x01},
        std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef},
    };

    require(sender.open(receiver->address(), receiver->port()).has_value(),
            "UDP sender must open against numeric loopback endpoint");
    require(sender.send(payload).has_value(),
            "UDP sender must accept one complete datagram");
    require(receiver->receive() == payload,
            "loopback receiver must observe the exact datagram bytes");
    sender.close();
    sender.close();
}

void enforces_lifecycle_and_validates_inputs() {
    detail::UdpSocket sender;
    const std::array payload{std::byte{0x01}};

    const auto send_while_closed = sender.send(payload);
    require(!send_while_closed &&
                send_while_closed.error().native_code == 0,
            "send while closed must return a non-native state issue");
    require(!sender.open("", 5004),
            "empty destination address must be rejected");
    require(!sender.open("localhost", 5004),
            "hostnames must not trigger DNS resolution");
    require(!sender.open("127.0.0.1", 0),
            "zero destination port must be rejected");

    require(sender.open("127.0.0.1", 5004).has_value(),
            "UDP sender must initialize its own platform socket runtime");
    sender.close();

    auto receiver = LoopbackReceiver::create(AF_INET);
    require(receiver.has_value(),
            "IPv4 loopback receiver must support lifecycle test");
    require(sender.open(receiver->address(), receiver->port()).has_value(),
            "closed UDP socket must open");
    require(!sender.open(receiver->address(), receiver->port()),
            "already-open UDP socket must reject duplicate open");
    require(!sender.send(std::span<const std::byte>{}),
            "empty UDP datagram must be rejected");

    std::vector<std::byte> oversized(65'508, std::byte{0x00});
    require(!sender.send(oversized),
            "UDP payload above the portable limit must be rejected");
    sender.close();
    require(sender.open(receiver->address(), receiver->port()).has_value(),
            "closed UDP socket must be reusable");
    sender.close();
}

}  // namespace

int main() {
    try {
        sends_complete_datagram_over_loopback(AF_INET);
        sends_complete_datagram_over_loopback(AF_INET6);
        enforces_lifecycle_and_validates_inputs();
    } catch (const std::exception& error) {
        std::cerr << "UDP socket test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "UDP socket tests passed\n";
    return EXIT_SUCCESS;
}
