#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace semilive::publisher::infra::output::detail {

struct H264RtpPacketizerConfig {
    std::uint8_t payload_type = 96;
    std::size_t max_datagram_bytes = 1200;
};

struct RtpSessionState {
    std::uint16_t next_sequence = 0;
    std::uint32_t initial_timestamp = 0;
    std::uint32_t ssrc = 0;
};

struct RtpPacketizationReceipt {
    std::uint64_t emitted_datagrams = 0;
    std::uint64_t emitted_bytes = 0;
};

using RtpDatagramEmitResult = std::expected<void, std::string>;
using RtpDatagramEmitter =
    std::function<RtpDatagramEmitResult(std::span<const std::byte>)>;
using RtpPacketizationResult =
    std::expected<RtpPacketizationReceipt, std::string>;

class H264RtpPacketizer {
public:
    explicit H264RtpPacketizer(H264RtpPacketizerConfig config);

    [[nodiscard]] RtpPacketizationResult packetize(
        std::span<const std::byte> annex_b,
        std::chrono::nanoseconds presentation_time,
        RtpSessionState& session,
        const RtpDatagramEmitter& emit_datagram);

private:
    H264RtpPacketizerConfig config_;
    std::vector<std::byte> datagram_;
};

}  // namespace semilive::publisher::infra::output::detail
