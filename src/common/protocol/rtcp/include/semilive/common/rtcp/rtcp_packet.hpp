#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace semilive::common::rtcp {

inline constexpr std::uint8_t protocol_version = 2;

enum class PacketType : std::uint8_t {
    SenderReport = 200,
    ReceiverReport = 201,
    SourceDescription = 202,
    Goodbye = 203,
    ApplicationDefined = 204,
    TransportLayerFeedback = 205,
    PayloadSpecificFeedback = 206,
};

struct ReceptionReportBlock {
    std::uint32_t source_ssrc = 0;
    std::uint8_t fraction_lost = 0;
    std::int32_t cumulative_lost = 0;
    std::uint32_t extended_highest_sequence = 0;
    std::uint32_t interarrival_jitter = 0;
    std::uint32_t last_sender_report = 0;
    std::uint32_t delay_since_last_sender_report = 0;

    bool operator==(const ReceptionReportBlock&) const = default;
};

struct SenderReport {
    std::uint32_t sender_ssrc = 0;
    std::uint64_t ntp_timestamp = 0;
    std::uint32_t rtp_timestamp = 0;
    std::uint32_t sender_packet_count = 0;
    std::uint32_t sender_octet_count = 0;
    std::vector<ReceptionReportBlock> reports;

    bool operator==(const SenderReport&) const = default;
};

struct ReceiverReport {
    std::uint32_t sender_ssrc = 0;
    std::vector<ReceptionReportBlock> reports;

    bool operator==(const ReceiverReport&) const = default;
};

struct SourceDescriptionChunk {
    std::uint32_t source_ssrc = 0;
    std::string canonical_name;

    bool operator==(const SourceDescriptionChunk&) const = default;
};

struct SourceDescription {
    std::vector<SourceDescriptionChunk> chunks;

    bool operator==(const SourceDescription&) const = default;
};

struct GenericNackBlock {
    std::uint16_t packet_id = 0;
    std::uint16_t lost_packet_bitmask = 0;

    bool operator==(const GenericNackBlock&) const = default;
};

struct GenericNack {
    std::uint32_t sender_ssrc = 0;
    std::uint32_t media_source_ssrc = 0;
    std::vector<GenericNackBlock> feedback;

    bool operator==(const GenericNack&) const = default;
};

struct UnknownPacket {
    std::uint8_t packet_type = 0;
    std::uint8_t count = 0;
    std::vector<std::byte> body;

    bool operator==(const UnknownPacket&) const = default;
};

using Packet = std::variant<SenderReport, ReceiverReport, SourceDescription,
                            GenericNack, UnknownPacket>;

struct CompoundPacket {
    std::vector<Packet> packets;

    bool operator==(const CompoundPacket&) const = default;
};

enum class ParseErrorCode : std::uint8_t {
    EmptyCompoundPacket,
    TruncatedHeader,
    UnsupportedVersion,
    InvalidLength,
    InvalidPadding,
    PaddingOnNonFinalPacket,
    InvalidPacketBody,
    InvalidSourceDescription,
};

struct ParseError {
    ParseErrorCode code = ParseErrorCode::InvalidPacketBody;
    std::size_t offset = 0;
    std::string message;
};

using ParseResult = std::expected<CompoundPacket, ParseError>;
using SerializeResult =
    std::expected<std::vector<std::byte>, std::string>;

[[nodiscard]] ParseResult parse_compound_packet(
    std::span<const std::byte> bytes);

[[nodiscard]] SerializeResult serialize_compound_packet(
    const CompoundPacket& compound);

// Sequence numbers must be supplied oldest first. Duplicate values are ignored.
// Unsigned 16-bit distance keeps a sequence such as 65535, 0, 1 contiguous.
[[nodiscard]] std::vector<GenericNackBlock> pack_generic_nack_blocks(
    std::span<const std::uint16_t> lost_sequences);

[[nodiscard]] std::vector<std::uint16_t> expand_generic_nack_blocks(
    std::span<const GenericNackBlock> feedback);

}  // namespace semilive::common::rtcp
