#include <semilive/receiver/domain/rtp/rtp_parser.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace semilive::receiver::domain {
namespace {

constexpr std::size_t fixed_header_size = 12;
constexpr std::size_t csrc_size = 4;
constexpr std::size_t extension_header_size = 4;
constexpr std::uint8_t supported_version = 2;

[[nodiscard]] std::uint8_t octet(const std::byte value) noexcept {
    return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] std::uint16_t read_u16_be(
    const std::vector<std::byte>& bytes,
    const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(octet(bytes[offset])) << 8U) |
        static_cast<std::uint16_t>(octet(bytes[offset + 1])));
}

[[nodiscard]] std::uint32_t read_u32_be(
    const std::vector<std::byte>& bytes,
    const std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(octet(bytes[offset])) << 24U) |
           (static_cast<std::uint32_t>(octet(bytes[offset + 1])) << 16U) |
           (static_cast<std::uint32_t>(octet(bytes[offset + 2])) << 8U) |
           static_cast<std::uint32_t>(octet(bytes[offset + 3]));
}

[[nodiscard]] RtpParseResult fail(const RtpParseErrorCode code,
                                  std::string message) {
    return std::unexpected{RtpParseIssue{code, std::move(message)}};
}

}  // namespace

RtpParseResult RtpParser::parse(model::UdpDatagram datagram) const {
    const auto datagram_size = datagram.bytes.size();
    if (datagram_size < fixed_header_size) {
        return fail(RtpParseErrorCode::DatagramTooShort,
                    "RTP datagram is shorter than the fixed header");
    }

    const auto first = octet(datagram.bytes[0]);
    const auto second = octet(datagram.bytes[1]);
    const auto version = static_cast<std::uint8_t>(first >> 6U);
    if (version != supported_version) {
        return fail(RtpParseErrorCode::UnsupportedVersion,
                    "RTP version must be 2");
    }

    const bool has_padding = (first & 0x20U) != 0;
    const bool has_extension = (first & 0x10U) != 0;
    const auto csrc_count = static_cast<std::size_t>(first & 0x0FU);
    std::size_t payload_offset = fixed_header_size;

    const auto csrc_bytes = csrc_count * csrc_size;
    if (datagram_size - payload_offset < csrc_bytes) {
        return fail(RtpParseErrorCode::TruncatedCsrcList,
                    "RTP CSRC list exceeds the datagram");
    }
    payload_offset += csrc_bytes;

    if (has_extension) {
        if (datagram_size - payload_offset < extension_header_size) {
            return fail(RtpParseErrorCode::TruncatedExtensionHeader,
                        "RTP extension header is truncated");
        }

        const auto extension_words = static_cast<std::size_t>(
            read_u16_be(datagram.bytes, payload_offset + 2));
        payload_offset += extension_header_size;
        const auto extension_bytes = extension_words * 4U;
        if (datagram_size - payload_offset < extension_bytes) {
            return fail(RtpParseErrorCode::TruncatedExtensionData,
                        "RTP extension data exceeds the datagram");
        }
        payload_offset += extension_bytes;
    }

    std::size_t padding_size = 0;
    if (has_padding) {
        padding_size = octet(datagram.bytes.back());
        const auto bytes_after_header = datagram_size - payload_offset;
        if (padding_size == 0 || padding_size > bytes_after_header) {
            return fail(RtpParseErrorCode::InvalidPadding,
                        "RTP padding length is invalid");
        }
    }

    const auto payload_size =
        datagram_size - payload_offset - padding_size;
    const bool marker = (second & 0x80U) != 0;
    const auto payload_type = static_cast<std::uint8_t>(second & 0x7FU);
    const auto sequence_number = read_u16_be(datagram.bytes, 2);
    const auto timestamp = read_u32_be(datagram.bytes, 4);
    const auto ssrc = read_u32_be(datagram.bytes, 8);

    return model::RtpPacket{
        std::move(datagram.bytes), datagram.received_at, marker,
        payload_type, sequence_number, timestamp, ssrc, payload_offset,
        payload_size};
}

}  // namespace semilive::receiver::domain
