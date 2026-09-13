#include <semilive/receiver/infrastructure/network/udp_datagram_source_backend.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
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

namespace semilive::receiver::infra::network {
namespace {

namespace contract = contracts::network;

constexpr std::size_t maximum_udp_payload_size = 65'507;

[[nodiscard]] contract::DatagramSourceIssue issue(
    const contract::DatagramSourceOperation operation,
    const std::int64_t native_code,
    std::string message) {
    return {operation, native_code, std::move(message)};
}

#if defined(_WIN32)
using NativeSocket = SOCKET;
using SocketLength = int;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;

[[nodiscard]] std::int64_t current_socket_error() noexcept {
    return static_cast<std::int64_t>(WSAGetLastError());
}

[[nodiscard]] bool interrupted(const std::int64_t error) noexcept {
    return error == WSAEINTR;
}

[[nodiscard]] bool would_block(const std::int64_t error) noexcept {
    return error == WSAEWOULDBLOCK;
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

[[nodiscard]] bool interrupted(const std::int64_t error) noexcept {
    return error == EINTR;
}

[[nodiscard]] bool would_block(const std::int64_t error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
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

[[nodiscard]] std::expected<ParsedEndpoint, contract::DatagramSourceIssue>
parse_endpoint(const std::string_view address,
               const std::uint16_t port) {
    if (address.empty()) {
        return std::unexpected{issue(
            contract::DatagramSourceOperation::Open, 0,
            "UDP bind address must not be empty")};
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
        contract::DatagramSourceOperation::Open, 0,
        "UDP bind address must be a numeric IPv4 or IPv6 address")};
}

[[nodiscard]] std::expected<void, contract::DatagramSourceIssue>
validate_config(const contract::DatagramSourceConfig& config) {
    if (config.maximum_datagram_bytes == 0 ||
        config.maximum_datagram_bytes > maximum_udp_payload_size) {
        return std::unexpected{issue(
            contract::DatagramSourceOperation::Open, 0,
            "UDP maximum datagram size must be in 1..65507")};
    }
    return {};
}

[[nodiscard]] std::expected<contract::DatagramSourceInfo,
                            contract::DatagramSourceIssue>
bound_endpoint(const NativeSocket socket,
               const std::size_t maximum_datagram_bytes) {
    sockaddr_storage storage{};
    SocketLength size = static_cast<SocketLength>(sizeof(storage));
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&storage), &size) !=
        0) {
        return std::unexpected{issue(
            contract::DatagramSourceOperation::Open,
            current_socket_error(),
            "failed to query bound UDP endpoint")};
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
            contract::DatagramSourceOperation::Open, 0,
            "bound UDP endpoint has an unsupported address family")};
    }

    char address_text[INET6_ADDRSTRLEN]{};
    if (inet_ntop(storage.ss_family, address, address_text,
                  static_cast<SocketLength>(sizeof(address_text))) ==
        nullptr) {
        return std::unexpected{issue(
            contract::DatagramSourceOperation::Open,
            current_socket_error(),
            "failed to format bound UDP address")};
    }
    return contract::DatagramSourceInfo{
        address_text, port, maximum_datagram_bytes};
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

struct UdpDatagramSourceBackend::Impl {
    using Clock = model::UdpDatagram::Clock;

    ~Impl() {
        close();
    }

    [[nodiscard]] contract::DatagramSourceOpenResult open(
        const contract::DatagramSourceConfig& config);
    [[nodiscard]] contract::DatagramSourceReceiveResult receive_for(
        std::chrono::milliseconds timeout);
    void close() noexcept;

    NativeSocket socket = invalid_socket;
    std::size_t maximum_datagram_bytes = 0;
    std::vector<std::byte> receive_buffer =
        std::vector<std::byte>(maximum_udp_payload_size);
#if defined(_WIN32)
    bool winsock_started = false;
#endif
};

contract::DatagramSourceOpenResult UdpDatagramSourceBackend::Impl::open(
    const contract::DatagramSourceConfig& config) {
    if (socket != invalid_socket) {
        return std::unexpected{issue(
            contract::DatagramSourceOperation::State, 0,
            "UDP datagram source can only open from the closed state")};
    }
    if (auto valid = validate_config(config); !valid) {
        return std::unexpected{std::move(valid.error())};
    }

#if defined(_WIN32)
    WSADATA data{};
    const auto startup_result = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup_result != 0) {
        return std::unexpected{issue(
            contract::DatagramSourceOperation::Open,
            static_cast<std::int64_t>(startup_result),
            "failed to initialize Winsock for UDP input")};
    }
    winsock_started = true;
#endif

    auto endpoint = parse_endpoint(config.bind_address, config.bind_port);
    if (!endpoint) {
        auto error = std::move(endpoint.error());
        close();
        return std::unexpected{std::move(error)};
    }

    socket = ::socket(endpoint->family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == invalid_socket) {
        const auto native_code = current_socket_error();
        close();
        return std::unexpected{issue(
            contract::DatagramSourceOperation::Open, native_code,
            "failed to create UDP input socket")};
    }
    if (::bind(socket,
               reinterpret_cast<const sockaddr*>(&endpoint->storage),
               endpoint->size) != 0) {
        const auto native_code = current_socket_error();
        close();
        return std::unexpected{issue(
            contract::DatagramSourceOperation::Open, native_code,
            "failed to bind UDP input socket")};
    }

    auto info = bound_endpoint(socket, config.maximum_datagram_bytes);
    if (!info) {
        auto error = std::move(info.error());
        close();
        return std::unexpected{std::move(error)};
    }
    maximum_datagram_bytes = config.maximum_datagram_bytes;
    return info;
}

contract::DatagramSourceReceiveResult
UdpDatagramSourceBackend::Impl::receive_for(
    const std::chrono::milliseconds timeout) {
    if (socket == invalid_socket) {
        return std::unexpected{issue(
            contract::DatagramSourceOperation::State, 0,
            "UDP datagram source must be open before receiving")};
    }
    if (timeout < std::chrono::milliseconds::zero()) {
        return std::unexpected{issue(
            contract::DatagramSourceOperation::Receive, 0,
            "UDP receive timeout must not be negative")};
    }

    const auto started_at = Clock::now();
    const auto deadline = started_at + timeout;
    bool first_attempt = true;
    while (first_attempt || Clock::now() < deadline) {
        first_attempt = false;
        const auto now = Clock::now();
        const auto remaining = timeout == std::chrono::milliseconds::zero()
                                   ? std::chrono::microseconds::zero()
                                   : std::chrono::ceil<std::chrono::microseconds>(
                                         std::max(Clock::duration::zero(),
                                                  deadline - now));
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
            const auto native_code = current_socket_error();
            if (interrupted(native_code)) {
                continue;
            }
            return std::unexpected{issue(
                contract::DatagramSourceOperation::Receive, native_code,
                "failed while waiting for a UDP datagram")};
        }
        if (selected == 0) {
            if (Clock::now() < deadline) {
                continue;
            }
            return contract::DatagramSourceObservation{
                contract::DatagramReceiveTimeout{}};
        }

#if defined(_WIN32)
        const auto received = ::recvfrom(
            socket, reinterpret_cast<char*>(receive_buffer.data()),
            static_cast<int>(receive_buffer.size()), 0, nullptr, nullptr);
        if (received == SOCKET_ERROR) {
#else
        const auto received = ::recvfrom(
            socket, receive_buffer.data(), receive_buffer.size(), 0,
            nullptr, nullptr);
        if (received == -1) {
#endif
            const auto native_code = current_socket_error();
            if (interrupted(native_code) || would_block(native_code)) {
                continue;
            }
            return std::unexpected{issue(
                contract::DatagramSourceOperation::Receive, native_code,
                "failed to receive UDP datagram")};
        }

        const auto received_size = static_cast<std::size_t>(received);
        if (received_size > maximum_datagram_bytes) {
            continue;
        }
        const auto received_end = receive_buffer.begin() +
                                  static_cast<std::ptrdiff_t>(received_size);
        std::vector<std::byte> bytes(receive_buffer.begin(), received_end);
        return contract::DatagramSourceObservation{model::UdpDatagram{
            std::move(bytes), Clock::now()}};
    }

    return contract::DatagramSourceObservation{
        contract::DatagramReceiveTimeout{}};
}

void UdpDatagramSourceBackend::Impl::close() noexcept {
    close_native_socket(socket);
    socket = invalid_socket;
    maximum_datagram_bytes = 0;
#if defined(_WIN32)
    if (winsock_started) {
        static_cast<void>(WSACleanup());
        winsock_started = false;
    }
#endif
}

UdpDatagramSourceBackend::UdpDatagramSourceBackend()
    : impl_{std::make_unique<Impl>()} {}

UdpDatagramSourceBackend::~UdpDatagramSourceBackend() = default;

contract::DatagramSourceOpenResult UdpDatagramSourceBackend::open(
    const contract::DatagramSourceConfig& config) {
    return impl_->open(config);
}

contract::DatagramSourceReceiveResult
UdpDatagramSourceBackend::receive_for(
    const std::chrono::milliseconds timeout) {
    return impl_->receive_for(timeout);
}

void UdpDatagramSourceBackend::close() noexcept {
    impl_->close();
}

}  // namespace semilive::receiver::infra::network
