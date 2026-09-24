#include <semilive/common/rtcp/udp_rtcp_transport.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
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
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace semilive::common::rtcp {
namespace {

constexpr std::size_t maximum_udp_payload_size = 65'507;

[[nodiscard]] TransportIssue issue(const TransportOperation operation,
                                   const std::int64_t native_code,
                                   std::string message) {
    return {operation, native_code, std::move(message)};
}

#if defined(_WIN32)
using NativeSocket = SOCKET;
using SocketLength = int;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
// MinGW headers do not expose Microsoft's SIO_UDP_CONNRESET symbol.
constexpr DWORD udp_connection_reset_control_code =
    _WSAIOW(IOC_VENDOR, 12);

[[nodiscard]] std::int64_t current_socket_error() noexcept {
    return static_cast<std::int64_t>(WSAGetLastError());
}

[[nodiscard]] bool interrupted(const std::int64_t code) noexcept {
    return code == WSAEINTR;
}

[[nodiscard]] bool would_block(const std::int64_t code) noexcept {
    return code == WSAEWOULDBLOCK;
}

void close_native_socket(const NativeSocket socket) noexcept {
    if (socket != invalid_socket) {
        static_cast<void>(closesocket(socket));
    }
}
#else
using NativeSocket = int;
using SocketLength = socklen_t;
constexpr NativeSocket invalid_socket = -1;

[[nodiscard]] std::int64_t current_socket_error() noexcept {
    return static_cast<std::int64_t>(errno);
}

[[nodiscard]] bool interrupted(const std::int64_t code) noexcept {
    return code == EINTR;
}

[[nodiscard]] bool would_block(const std::int64_t code) noexcept {
    return code == EAGAIN || code == EWOULDBLOCK;
}

void close_native_socket(const NativeSocket socket) noexcept {
    if (socket != invalid_socket) {
        static_cast<void>(::close(socket));
    }
}
#endif

struct ParsedEndpoint {
    sockaddr_storage storage{};
    SocketLength size = 0;
    int family = AF_UNSPEC;
};

[[nodiscard]] std::expected<ParsedEndpoint, TransportIssue> parse_endpoint(
    const std::string_view address,
    const std::uint16_t port,
    const std::string_view label) {
    if (address.empty()) {
        return std::unexpected{issue(
            TransportOperation::Open, 0,
            std::string{label} + " address must not be empty")};
    }

    const std::string address_text{address};
    ParsedEndpoint endpoint;
    sockaddr_in ipv4{};
    if (inet_pton(AF_INET, address_text.c_str(), &ipv4.sin_addr) == 1) {
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(port);
        endpoint.size = static_cast<SocketLength>(sizeof(ipv4));
        endpoint.family = AF_INET;
        std::memcpy(&endpoint.storage, &ipv4, sizeof(ipv4));
        return endpoint;
    }

    sockaddr_in6 ipv6{};
    if (inet_pton(AF_INET6, address_text.c_str(), &ipv6.sin6_addr) == 1) {
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(port);
        endpoint.size = static_cast<SocketLength>(sizeof(ipv6));
        endpoint.family = AF_INET6;
        std::memcpy(&endpoint.storage, &ipv6, sizeof(ipv6));
        return endpoint;
    }

    return std::unexpected{issue(
        TransportOperation::Open, 0,
        std::string{label} +
            " address must be a numeric IPv4 or IPv6 address")};
}

[[nodiscard]] std::expected<void, TransportIssue> validate_config(
    const TransportConfig& config) {
    if (config.peer_port == 0) {
        return std::unexpected{issue(
            TransportOperation::Open, 0,
            "RTCP peer port must be in 1..65535")};
    }
    if (config.maximum_datagram_bytes < 4U ||
        config.maximum_datagram_bytes > maximum_udp_payload_size) {
        return std::unexpected{issue(
            TransportOperation::Open, 0,
            "RTCP maximum datagram size must be in 4..65507")};
    }
    if (config.receive_buffer_bytes == 0U ||
        config.receive_buffer_bytes > static_cast<std::size_t>(INT_MAX)) {
        return std::unexpected{issue(
            TransportOperation::Open, 0,
            "RTCP receive buffer size must be in 1..2147483647")};
    }
    return {};
}

[[nodiscard]] std::expected<std::size_t, TransportIssue>
configure_receive_buffer(const NativeSocket socket,
                         const std::size_t requested_bytes) {
    const auto requested = static_cast<int>(requested_bytes);
#if defined(_WIN32)
    const auto set_result = setsockopt(
        socket, SOL_SOCKET, SO_RCVBUF,
        reinterpret_cast<const char*>(&requested),
        static_cast<int>(sizeof(requested)));
#else
    const auto set_result = setsockopt(
        socket, SOL_SOCKET, SO_RCVBUF, &requested,
        static_cast<socklen_t>(sizeof(requested)));
#endif
    if (set_result != 0) {
        return std::unexpected{issue(
            TransportOperation::Open, current_socket_error(),
            "failed to configure RTCP UDP receive buffer")};
    }

    int actual = 0;
#if defined(_WIN32)
    int option_size = static_cast<int>(sizeof(actual));
    const auto get_result = getsockopt(
        socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&actual),
        &option_size);
#else
    socklen_t option_size = static_cast<socklen_t>(sizeof(actual));
    const auto get_result =
        getsockopt(socket, SOL_SOCKET, SO_RCVBUF, &actual, &option_size);
#endif
    if (get_result != 0 || actual <= 0) {
        return std::unexpected{issue(
            TransportOperation::Open, current_socket_error(),
            "failed to query RTCP UDP receive buffer")};
    }
    return static_cast<std::size_t>(actual);
}

[[nodiscard]] std::expected<void, TransportIssue>
configure_unreachable_peer_behavior(const NativeSocket socket) {
#if defined(_WIN32)
    BOOL report_connection_resets = FALSE;
    DWORD bytes_returned = 0;
    if (WSAIoctl(socket, udp_connection_reset_control_code,
                 &report_connection_resets,
                 static_cast<DWORD>(sizeof(report_connection_resets)),
                 nullptr, 0, &bytes_returned, nullptr, nullptr) ==
        SOCKET_ERROR) {
        return std::unexpected{issue(
            TransportOperation::Open, current_socket_error(),
            "failed to disable RTCP UDP connection reset notifications")};
    }
#else
    static_cast<void>(socket);
#endif
    return {};
}

[[nodiscard]] std::expected<std::pair<std::string, std::uint16_t>,
                            TransportIssue>
bound_endpoint(const NativeSocket socket) {
    sockaddr_storage storage{};
    SocketLength size = static_cast<SocketLength>(sizeof(storage));
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&storage), &size) !=
        0) {
        return std::unexpected{issue(
            TransportOperation::Open, current_socket_error(),
            "failed to query bound RTCP UDP endpoint")};
    }

    const void* address = nullptr;
    std::uint16_t port = 0;
    if (storage.ss_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&storage);
        address = &ipv4->sin_addr;
        port = ntohs(ipv4->sin_port);
    } else if (storage.ss_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&storage);
        address = &ipv6->sin6_addr;
        port = ntohs(ipv6->sin6_port);
    } else {
        return std::unexpected{issue(
            TransportOperation::Open, 0,
            "RTCP UDP socket bound an unsupported address family")};
    }

    char address_text[INET6_ADDRSTRLEN]{};
    if (inet_ntop(storage.ss_family, address, address_text,
                  static_cast<SocketLength>(sizeof(address_text))) ==
        nullptr) {
        return std::unexpected{issue(
            TransportOperation::Open, current_socket_error(),
            "failed to format bound RTCP UDP endpoint")};
    }
    return std::pair<std::string, std::uint16_t>{address_text, port};
}

[[nodiscard]] timeval select_timeout(
    const std::chrono::microseconds remaining) noexcept {
    using Seconds = std::chrono::seconds;
    const auto seconds = std::chrono::duration_cast<Seconds>(remaining);
    const auto bounded_seconds = std::min<std::int64_t>(
        seconds.count(), static_cast<std::int64_t>(LONG_MAX));
    const auto consumed = Seconds{bounded_seconds};
    const auto microseconds = remaining - consumed;
    return {
        static_cast<long>(bounded_seconds),
        bounded_seconds == seconds.count()
            ? static_cast<long>(microseconds.count())
            : 0L,
    };
}

}  // namespace

struct UdpTransport::Impl {
    ~Impl() {
        close();
    }

    [[nodiscard]] TransportOpenResult open(const TransportConfig& config);
    [[nodiscard]] TransportReceiveResult receive_for(
        std::chrono::milliseconds timeout);
    [[nodiscard]] TransportSendResult send(
        std::span<const std::byte> datagram);
    void close() noexcept;

    NativeSocket socket = invalid_socket;
    ParsedEndpoint peer;
    std::size_t maximum_datagram_bytes = 0;
    std::vector<std::byte> receive_buffer =
        std::vector<std::byte>(maximum_udp_payload_size);
#if defined(_WIN32)
    bool winsock_started = false;
#endif
};

TransportOpenResult UdpTransport::Impl::open(const TransportConfig& config) {
    if (socket != invalid_socket) {
        return std::unexpected{issue(
            TransportOperation::State, 0,
            "RTCP UDP transport can only open from the closed state")};
    }
    if (auto valid = validate_config(config); !valid) {
        return std::unexpected{std::move(valid.error())};
    }

#if defined(_WIN32)
    WSADATA data{};
    const auto startup_result = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup_result != 0) {
        return std::unexpected{issue(
            TransportOperation::Open,
            static_cast<std::int64_t>(startup_result),
            "failed to initialize Winsock for RTCP")};
    }
    winsock_started = true;
#endif

    auto bind_endpoint =
        parse_endpoint(config.bind_address, config.bind_port, "RTCP bind");
    auto parsed_peer =
        parse_endpoint(config.peer_address, config.peer_port, "RTCP peer");
    if (!bind_endpoint || !parsed_peer) {
        auto error = !bind_endpoint ? std::move(bind_endpoint.error())
                                    : std::move(parsed_peer.error());
        close();
        return std::unexpected{std::move(error)};
    }
    if (bind_endpoint->family != parsed_peer->family) {
        close();
        return std::unexpected{issue(
            TransportOperation::Open, 0,
            "RTCP bind and peer addresses must use the same IP family")};
    }

    socket = ::socket(bind_endpoint->family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == invalid_socket) {
        const auto code = current_socket_error();
        close();
        return std::unexpected{issue(
            TransportOperation::Open, code,
            "failed to create RTCP UDP socket")};
    }
    if (auto configured = configure_unreachable_peer_behavior(socket);
        !configured) {
        auto error = std::move(configured.error());
        close();
        return std::unexpected{std::move(error)};
    }
    auto receive_buffer_size =
        configure_receive_buffer(socket, config.receive_buffer_bytes);
    if (!receive_buffer_size) {
        auto error = std::move(receive_buffer_size.error());
        close();
        return std::unexpected{std::move(error)};
    }
    if (::bind(socket,
               reinterpret_cast<const sockaddr*>(&bind_endpoint->storage),
               bind_endpoint->size) != 0) {
        const auto code = current_socket_error();
        close();
        return std::unexpected{issue(
            TransportOperation::Open, code,
            "failed to bind RTCP UDP socket")};
    }

    auto endpoint = bound_endpoint(socket);
    if (!endpoint) {
        auto error = std::move(endpoint.error());
        close();
        return std::unexpected{std::move(error)};
    }
    peer = *parsed_peer;
    maximum_datagram_bytes = config.maximum_datagram_bytes;
    return TransportInfo{endpoint->first, endpoint->second,
                         maximum_datagram_bytes, *receive_buffer_size};
}

TransportReceiveResult UdpTransport::Impl::receive_for(
    const std::chrono::milliseconds timeout) {
    if (socket == invalid_socket) {
        return std::unexpected{issue(
            TransportOperation::State, 0,
            "RTCP UDP transport must be open before receiving")};
    }
    if (timeout < std::chrono::milliseconds::zero()) {
        return std::unexpected{issue(
            TransportOperation::Receive, 0,
            "RTCP receive timeout must not be negative")};
    }

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + timeout;
    bool first_attempt = true;
    while (first_attempt || Clock::now() < deadline) {
        first_attempt = false;
        const auto remaining =
            timeout == std::chrono::milliseconds::zero()
                ? std::chrono::microseconds::zero()
                : std::chrono::ceil<std::chrono::microseconds>(
                      std::max(Clock::duration::zero(),
                               deadline - Clock::now()));
        auto native_timeout = select_timeout(remaining);
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket, &readable);
#if defined(_WIN32)
        const auto selected =
            select(0, &readable, nullptr, nullptr, &native_timeout);
        if (selected == SOCKET_ERROR) {
#else
        const auto selected =
            select(socket + 1, &readable, nullptr, nullptr, &native_timeout);
        if (selected == -1) {
#endif
            const auto code = current_socket_error();
            if (interrupted(code)) {
                continue;
            }
            return std::unexpected{issue(
                TransportOperation::Receive, code,
                "failed while waiting for an RTCP UDP datagram")};
        }
        if (selected == 0) {
            if (Clock::now() < deadline) {
                continue;
            }
            return std::optional<ReceivedDatagram>{};
        }

#if defined(_WIN32)
        const auto received = ::recvfrom(
            socket, reinterpret_cast<char*>(receive_buffer.data()),
            static_cast<int>(receive_buffer.size()), 0, nullptr, nullptr);
        if (received == SOCKET_ERROR) {
#else
        const auto received = ::recvfrom(
            socket, receive_buffer.data(), receive_buffer.size(), 0, nullptr,
            nullptr);
        if (received == -1) {
#endif
            const auto code = current_socket_error();
            if (interrupted(code) || would_block(code)) {
                continue;
            }
            return std::unexpected{issue(
                TransportOperation::Receive, code,
                "failed to receive an RTCP UDP datagram")};
        }

        const auto size = static_cast<std::size_t>(received);
        if (size > maximum_datagram_bytes) {
            continue;
        }
        const auto end = receive_buffer.begin() +
                         static_cast<std::ptrdiff_t>(size);
        return std::optional<ReceivedDatagram>{std::in_place,
                                               std::vector<std::byte>{
                                                   receive_buffer.begin(), end},
                                               Clock::now()};
    }
    return std::optional<ReceivedDatagram>{};
}

TransportSendResult UdpTransport::Impl::send(
    const std::span<const std::byte> datagram) {
    if (socket == invalid_socket) {
        return std::unexpected{issue(
            TransportOperation::State, 0,
            "RTCP UDP transport must be open before sending")};
    }
    if (datagram.empty() || datagram.size() > maximum_udp_payload_size) {
        return std::unexpected{issue(
            TransportOperation::Send, 0,
            "RTCP datagram size must be in 1..65507")};
    }

#if defined(_WIN32)
    const auto sent = ::sendto(
        socket, reinterpret_cast<const char*>(datagram.data()),
        static_cast<int>(datagram.size()), 0,
        reinterpret_cast<const sockaddr*>(&peer.storage), peer.size);
    if (sent == SOCKET_ERROR) {
#else
    constexpr int send_flags =
#if defined(MSG_NOSIGNAL)
        MSG_NOSIGNAL;
#else
        0;
#endif
    const auto sent = ::sendto(
        socket, datagram.data(), datagram.size(), send_flags,
        reinterpret_cast<const sockaddr*>(&peer.storage), peer.size);
    if (sent == -1) {
#endif
        return std::unexpected{issue(
            TransportOperation::Send, current_socket_error(),
            "failed to send an RTCP UDP datagram")};
    }
    if (static_cast<std::size_t>(sent) != datagram.size()) {
        return std::unexpected{issue(
            TransportOperation::Send, 0,
            "RTCP UDP transport sent an incomplete datagram")};
    }
    return {};
}

void UdpTransport::Impl::close() noexcept {
    close_native_socket(socket);
    socket = invalid_socket;
    peer = {};
    maximum_datagram_bytes = 0;
#if defined(_WIN32)
    if (winsock_started) {
        static_cast<void>(WSACleanup());
        winsock_started = false;
    }
#endif
}

UdpTransport::UdpTransport() : impl_{std::make_unique<Impl>()} {}

UdpTransport::~UdpTransport() = default;

TransportOpenResult UdpTransport::open(const TransportConfig& config) {
    return impl_->open(config);
}

TransportReceiveResult UdpTransport::receive_for(
    const std::chrono::milliseconds timeout) {
    return impl_->receive_for(timeout);
}

TransportSendResult UdpTransport::send(
    const std::span<const std::byte> datagram) {
    return impl_->send(datagram);
}

void UdpTransport::close() noexcept {
    impl_->close();
}

}  // namespace semilive::common::rtcp
