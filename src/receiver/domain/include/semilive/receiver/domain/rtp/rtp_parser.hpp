#pragma once

#include <semilive/receiver/model/network/udp_datagram.hpp>
#include <semilive/receiver/model/rtp/rtp_packet.hpp>

#include <cstdint>
#include <expected>
#include <string>

namespace semilive::receiver::domain {

enum class RtpParseErrorCode : std::uint8_t {
    DatagramTooShort,
    UnsupportedVersion,
    TruncatedCsrcList,
    TruncatedExtensionHeader,
    TruncatedExtensionData,
    InvalidPadding,
};

struct RtpParseIssue {
    RtpParseErrorCode code = RtpParseErrorCode::DatagramTooShort;
    std::string message;
};

using RtpParseResult = std::expected<model::RtpPacket, RtpParseIssue>;

class RtpParser final {
public:
    [[nodiscard]] RtpParseResult parse(model::UdpDatagram datagram) const;
};

}  // namespace semilive::receiver::domain
