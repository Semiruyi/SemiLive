#include <semilive/publisher/infrastructure/output/rtp_udp_video_output_backend.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
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
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace {

namespace contracts = semilive::publisher::contracts::output;
namespace model = semilive::publisher::model;

using semilive::publisher::infra::output::RtpUdpVideoOutputBackend;
using semilive::publisher::infra::output::RtpUdpVideoOutputConfig;

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

[[nodiscard]] std::uint8_t value(const std::byte byte) noexcept {
    return std::to_integer<std::uint8_t>(byte);
}

[[nodiscard]] std::uint16_t read_u16(const std::vector<std::byte>& bytes,
                                     const std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(value(bytes.at(offset))) << 8U) |
        value(bytes.at(offset + 1)));
}

[[nodiscard]] std::uint32_t read_u32(const std::vector<std::byte>& bytes,
                                     const std::size_t offset) {
    return (static_cast<std::uint32_t>(value(bytes.at(offset))) << 24U) |
           (static_cast<std::uint32_t>(value(bytes.at(offset + 1))) << 16U) |
           (static_cast<std::uint32_t>(value(bytes.at(offset + 2))) << 8U) |
           value(bytes.at(offset + 3));
}

class Ipv4LoopbackReceiver {
public:
    Ipv4LoopbackReceiver() {
#if defined(_WIN32)
        WSADATA data{};
        const auto startup_result = WSAStartup(MAKEWORD(2, 2), &data);
        require(startup_result == 0,
                "Winsock must initialize for backend loopback test");
        winsock_started_ = true;
#endif
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(socket_ != invalid_socket,
                "IPv4 receiver socket must be created");

        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = 0;
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(socket_, reinterpret_cast<const sockaddr*>(&endpoint),
                       sizeof(endpoint)) == 0,
                "IPv4 receiver socket must bind to loopback");

        socklen_t endpoint_size = sizeof(endpoint);
        require(getsockname(socket_, reinterpret_cast<sockaddr*>(&endpoint),
                            &endpoint_size) == 0,
                "IPv4 receiver must report its assigned port");
        port_ = ntohs(endpoint.sin_port);

#if defined(_WIN32)
        const DWORD timeout_milliseconds = 2000;
        require(setsockopt(
                    socket_, SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char*>(&timeout_milliseconds),
                    sizeof(timeout_milliseconds)) == 0,
                "IPv4 receiver timeout must be configured");
#else
        const timeval timeout{2, 0};
        require(setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof(timeout)) == 0,
                "IPv4 receiver timeout must be configured");
#endif
    }

    ~Ipv4LoopbackReceiver() {
        if (socket_ != invalid_socket) {
#if defined(_WIN32)
            static_cast<void>(closesocket(socket_));
#else
            static_cast<void>(::close(socket_));
#endif
        }
#if defined(_WIN32)
        if (winsock_started_) {
            static_cast<void>(WSACleanup());
        }
#endif
    }

    Ipv4LoopbackReceiver(const Ipv4LoopbackReceiver&) = delete;
    Ipv4LoopbackReceiver& operator=(const Ipv4LoopbackReceiver&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    [[nodiscard]] std::vector<std::byte> receive() const {
        std::array<std::byte, 1200> buffer{};
#if defined(_WIN32)
        const auto received = ::recv(
            socket_, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0);
        require(received != SOCKET_ERROR,
                "backend loopback receiver timed out or failed");
#else
        const auto received = ::recv(socket_, buffer.data(), buffer.size(), 0);
        require(received != -1,
                "backend loopback receiver timed out or failed");
#endif
        return {buffer.begin(), buffer.begin() + received};
    }

private:
    NativeSocket socket_ = invalid_socket;
    std::uint16_t port_ = 0;
#if defined(_WIN32)
    bool winsock_started_ = false;
#endif
};

[[nodiscard]] model::EncodedVideoAccessUnit access_unit(
    std::vector<std::byte> annex_b,
    const std::chrono::nanoseconds presentation_time) {
    return {
        std::move(annex_b),
        presentation_time,
        true,
        1,
        std::chrono::steady_clock::now(),
    };
}

void sends_packetized_access_unit_and_reports_udp_receipt() {
    Ipv4LoopbackReceiver receiver;
    RtpUdpVideoOutputBackend backend{{"127.0.0.1", receiver.port(), 96, 18}};

    const auto opened = backend.open();
    require(opened &&
                opened->output_name ==
                    "rtp+udp://127.0.0.1:" +
                        std::to_string(receiver.port()) + "?pt=96",
            "open must report the configured RTP/UDP endpoint");

    const auto encoded = access_unit(
        {
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x01}, std::byte{0x67}, std::byte{0x11},
            std::byte{0x80},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
            std::byte{0x65}, std::byte{0x10}, std::byte{0x11},
            std::byte{0x12}, std::byte{0x13}, std::byte{0x14},
            std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
            std::byte{0x80},
        },
        std::chrono::milliseconds{1});
    const auto consumed = backend.consume(encoded);
    require(consumed && consumed->emitted_units == 4 &&
                consumed->emitted_bytes == 66,
            "consume must report every emitted RTP/UDP datagram and byte");

    std::vector<std::vector<std::byte>> datagrams;
    for (std::size_t index = 0; index < 4; ++index) {
        datagrams.push_back(receiver.receive());
    }
    require(datagrams[0].size() == 15 && datagrams[1].size() == 18 &&
                datagrams[2].size() == 18 && datagrams[3].size() == 15,
            "loopback must receive the expected Single NAL and FU-A sizes");
    const auto first_sequence = read_u16(datagrams[0], 2);
    const auto first_timestamp = read_u32(datagrams[0], 4);
    const auto session_ssrc = read_u32(datagrams[0], 8);
    for (std::size_t index = 0; index < datagrams.size(); ++index) {
        require(value(datagrams[index][0]) == 0x80,
                "backend must emit RTP version 2 without extensions");
        require(read_u16(datagrams[index], 2) ==
                    static_cast<std::uint16_t>(first_sequence + index),
                "backend must preserve continuous RTP sequence numbers");
        require(read_u32(datagrams[index], 4) == first_timestamp,
                "all packets in one AU must share one RTP timestamp");
        require(read_u32(datagrams[index], 8) == session_ssrc,
                "backend must preserve one SSRC for the session");
    }
    require((value(datagrams[0][1]) & 0x80U) == 0 &&
                (value(datagrams[1][1]) & 0x80U) == 0 &&
                (value(datagrams[2][1]) & 0x80U) == 0 &&
                value(datagrams[3][1]) == 0xe0,
            "only the final RTP packet of the access unit must set Marker");

    const auto next_encoded = access_unit(
        {std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
         std::byte{0x61}, std::byte{0x33}, std::byte{0x80}},
        std::chrono::milliseconds{2});
    const auto next_consumed = backend.consume(next_encoded);
    require(next_consumed && next_consumed->emitted_units == 1 &&
                next_consumed->emitted_bytes == 15,
            "second AU must emit one complete Single NAL datagram");
    const auto next_datagram = receiver.receive();
    require(read_u16(next_datagram, 2) ==
                static_cast<std::uint16_t>(first_sequence + 4U),
            "RTP sequence must remain continuous across access units");
    require(read_u32(next_datagram, 4) - first_timestamp == 90,
            "one millisecond of media time must advance RTP by 90 ticks");
    require(read_u32(next_datagram, 8) == session_ssrc,
            "SSRC must remain stable across access units in one session");
    require(value(next_datagram[1]) == 0xe0,
            "single packet second AU must carry the Marker bit");

    const auto flushed = backend.flush();
    require(flushed && flushed->emitted_units == 0 &&
                flushed->emitted_bytes == 0,
            "RTP flush must not emit protocol data");
    require(!backend.flush(), "RTP session must only flush once");
    require(!backend.consume(encoded),
            "flushed RTP session must reject additional access units");
    backend.close();
    backend.close();
}

void maps_input_failures_and_requires_close_before_reuse() {
    Ipv4LoopbackReceiver receiver;
    RtpUdpVideoOutputBackend backend{{"127.0.0.1", receiver.port(), 96, 1200}};
    const auto valid = access_unit(
        {std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
         std::byte{0x61}, std::byte{0x80}},
        std::chrono::nanoseconds{0});

    const auto closed_consume = backend.consume(valid);
    require(!closed_consume &&
                closed_consume.error().operation ==
                    contracts::VideoOutputOperation::State,
            "consume while closed must report a state issue");

    require(backend.open().has_value(), "backend must open for input test");
    const auto empty = backend.consume(
        access_unit({}, std::chrono::nanoseconds{0}));
    require(!empty &&
                empty.error().operation ==
                    contracts::VideoOutputOperation::Consume &&
                empty.error().native_code == 0 &&
                empty.error().message.find("after 0 RTP datagram(s)") !=
                    std::string::npos,
            "empty AU must become a structured consume failure");
    require(!backend.consume(valid) && !backend.flush(),
            "failed backend must require close before reuse");
    backend.close();

    require(backend.open().has_value(),
            "closed failed backend must open a new session");
    const auto negative = backend.consume(
        access_unit(valid.annex_b, std::chrono::nanoseconds{-1}));
    require(!negative &&
                negative.error().operation ==
                    contracts::VideoOutputOperation::Consume,
            "negative presentation time must become a consume failure");
    backend.close();
}

void validates_configuration_and_production_random_source() {
    const std::array invalid_configs{
        RtpUdpVideoOutputConfig{"", 5004, 96, 1200},
        RtpUdpVideoOutputConfig{"localhost", 5004, 96, 1200},
        RtpUdpVideoOutputConfig{"127.0.0.1", 0, 96, 1200},
        RtpUdpVideoOutputConfig{"127.0.0.1", 5004, 95, 1200},
        RtpUdpVideoOutputConfig{"127.0.0.1", 5004, 128, 1200},
        RtpUdpVideoOutputConfig{"127.0.0.1", 5004, 96, 14},
        RtpUdpVideoOutputConfig{"127.0.0.1", 5004, 96, 65'508},
    };
    for (const auto& config : invalid_configs) {
        RtpUdpVideoOutputBackend backend{config};
        const auto opened = backend.open();
        require(!opened &&
                    opened.error().operation ==
                        contracts::VideoOutputOperation::Open,
                "invalid RTP/UDP config must report an open issue");
    }

    Ipv4LoopbackReceiver receiver;
    RtpUdpVideoOutputBackend backend{{"127.0.0.1", receiver.port(), 127,
                                      1200}};
    const auto opened = backend.open();
    require(opened.has_value(),
            "production RTP session must open with system-generated state");
    const auto duplicate = backend.open();
    require(!duplicate &&
                duplicate.error().operation ==
                    contracts::VideoOutputOperation::State,
            "duplicate open must report a state issue");
    backend.close();
}

}  // namespace

int main() {
    try {
        sends_packetized_access_unit_and_reports_udp_receipt();
        maps_input_failures_and_requires_close_before_reuse();
        validates_configuration_and_production_random_source();
    } catch (const std::exception& error) {
        std::cerr << "RTP/UDP video output backend test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "RTP/UDP video output backend tests passed\n";
    return EXIT_SUCCESS;
}
