#include "udp_socket.hpp"

#include <cerrno>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace semilive::publisher::infra::output::detail {
namespace {

constexpr std::size_t maximum_udp_payload_size = 65'507;

[[nodiscard]] UdpSocketIssue issue(const std::int64_t native_code,
                                   std::string message) {
    return {native_code, std::move(message)};
}

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;

[[nodiscard]] std::int64_t current_socket_error() noexcept {
    return static_cast<std::int64_t>(WSAGetLastError());
}

void close_native_socket(const NativeSocket socket) noexcept {
    if (socket != invalid_socket) {
        static_cast<void>(closesocket(socket));
    }
}
#else
using NativeSocket = int;
constexpr NativeSocket invalid_socket = -1;

[[nodiscard]] std::int64_t current_socket_error() noexcept {
    return static_cast<std::int64_t>(errno);
}

void close_native_socket(const NativeSocket socket) noexcept {
    if (socket != invalid_socket) {
        static_cast<void>(::close(socket));
    }
}
#endif

struct ParsedEndpoint {
    sockaddr_storage storage{};
    socklen_t size = 0;
    int family = AF_UNSPEC;
};

[[nodiscard]] std::expected<ParsedEndpoint, UdpSocketIssue> parse_endpoint(
    const std::string_view address,
    const std::uint16_t port) {
    if (address.empty()) {
        return std::unexpected{
            issue(0, "UDP destination address must not be empty")};
    }
    if (port == 0) {
        return std::unexpected{
            issue(0, "UDP destination port must not be zero")};
    }

    const std::string address_text{address};
    ParsedEndpoint endpoint;
    sockaddr_in ipv4{};
    if (inet_pton(AF_INET, address_text.c_str(), &ipv4.sin_addr) == 1) {
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(port);
        endpoint.size = static_cast<socklen_t>(sizeof(ipv4));
        endpoint.family = AF_INET;
        std::memcpy(&endpoint.storage, &ipv4, sizeof(ipv4));
        return endpoint;
    }

    sockaddr_in6 ipv6{};
    if (inet_pton(AF_INET6, address_text.c_str(), &ipv6.sin6_addr) == 1) {
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(port);
        endpoint.size = static_cast<socklen_t>(sizeof(ipv6));
        endpoint.family = AF_INET6;
        std::memcpy(&endpoint.storage, &ipv6, sizeof(ipv6));
        return endpoint;
    }

    return std::unexpected{
        issue(0, "UDP destination must be a numeric IPv4 or IPv6 address")};
}

[[nodiscard]] UdpSocketResult make_nonblocking(
    const NativeSocket socket) {
#if defined(_WIN32)
    u_long enabled = 1;
    if (ioctlsocket(socket, static_cast<long>(FIONBIO), &enabled) ==
        SOCKET_ERROR) {
        return std::unexpected{issue(
            current_socket_error(), "failed to make UDP socket non-blocking")};
    }
#else
    const auto current_flags = fcntl(socket, F_GETFL, 0);
    if (current_flags == -1) {
        return std::unexpected{issue(
            current_socket_error(), "failed to read UDP socket flags")};
    }
    if (fcntl(socket, F_SETFL, current_flags | O_NONBLOCK) == -1) {
        return std::unexpected{issue(
            current_socket_error(), "failed to make UDP socket non-blocking")};
    }
#endif
    return {};
}

}  // namespace

struct UdpSocket::Impl {
    NativeSocket socket = invalid_socket;
#if defined(_WIN32)
    bool winsock_started = false;
#endif
};

UdpSocket::UdpSocket() : impl_{std::make_unique<Impl>()} {}

UdpSocket::~UdpSocket() {
    close();
}

UdpSocketResult UdpSocket::open(const std::string_view destination_address,
                                const std::uint16_t destination_port) {
    if (impl_->socket != invalid_socket) {
        return std::unexpected{
            issue(0, "UDP socket can only open from the closed state")};
    }

#if defined(_WIN32)
    WSADATA data{};
    const auto startup_result = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup_result != 0) {
        return std::unexpected{issue(
            static_cast<std::int64_t>(startup_result),
            "failed to initialize Winsock for UDP output")};
    }
    impl_->winsock_started = true;
#endif

    auto endpoint = parse_endpoint(destination_address, destination_port);
    if (!endpoint) {
        const auto error = std::move(endpoint.error());
        close();
        return std::unexpected{error};
    }

    impl_->socket =
        ::socket(endpoint->family, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket == invalid_socket) {
        const auto native_code = current_socket_error();
        close();
        return std::unexpected{
            issue(native_code, "failed to create UDP socket")};
    }

    if (::connect(impl_->socket,
                  reinterpret_cast<const sockaddr*>(&endpoint->storage),
                  endpoint->size) != 0) {
        const auto native_code = current_socket_error();
        close();
        return std::unexpected{
            issue(native_code, "failed to set UDP destination")};
    }

    if (auto nonblocking = make_nonblocking(impl_->socket); !nonblocking) {
        const auto error = std::move(nonblocking.error());
        close();
        return std::unexpected{error};
    }
    return {};
}

UdpSocketResult UdpSocket::send(
    const std::span<const std::byte> datagram) {
    if (impl_->socket == invalid_socket) {
        return std::unexpected{
            issue(0, "UDP socket must be open before sending")};
    }
    if (datagram.empty()) {
        return std::unexpected{
            issue(0, "UDP datagram must not be empty")};
    }
    if (datagram.size() > maximum_udp_payload_size) {
        return std::unexpected{
            issue(0, "UDP datagram exceeds the payload limit")};
    }

#if defined(_WIN32)
    const auto sent = ::send(
        impl_->socket, reinterpret_cast<const char*>(datagram.data()),
        static_cast<int>(datagram.size()), 0);
    if (sent == SOCKET_ERROR) {
#else
    constexpr int send_flags =
#if defined(MSG_NOSIGNAL)
        MSG_NOSIGNAL;
#else
        0;
#endif
    const auto sent = ::send(impl_->socket, datagram.data(), datagram.size(),
                             send_flags);
    if (sent == -1) {
#endif
        return std::unexpected{
            issue(current_socket_error(), "failed to send UDP datagram")};
    }

    if (static_cast<std::size_t>(sent) != datagram.size()) {
        return std::unexpected{
            issue(0, "UDP socket sent an incomplete datagram")};
    }
    return {};
}

void UdpSocket::close() noexcept {
    close_native_socket(impl_->socket);
    impl_->socket = invalid_socket;
#if defined(_WIN32)
    if (impl_->winsock_started) {
        static_cast<void>(WSACleanup());
        impl_->winsock_started = false;
    }
#endif
}

}  // namespace semilive::publisher::infra::output::detail
