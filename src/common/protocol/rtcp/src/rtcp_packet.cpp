#include <semilive/common/rtcp/rtcp_packet.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace semilive::common::rtcp {
namespace {

constexpr std::size_t header_size = 4;
constexpr std::size_t reception_report_size = 24;
constexpr std::size_t sender_report_fixed_size = 28;
constexpr std::size_t receiver_report_fixed_size = 8;
constexpr std::int32_t minimum_signed_24 = -8'388'608;
constexpr std::int32_t maximum_signed_24 = 8'388'607;
constexpr std::size_t maximum_count = 31;
constexpr std::size_t maximum_packet_size =
    (static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) +
     1U) *
    4U;

[[nodiscard]] ParseError parse_error(const ParseErrorCode code,
                                     const std::size_t offset,
                                     std::string message) {
    return {code, offset, std::move(message)};
}

[[nodiscard]] std::uint16_t read_u16(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(bytes[offset]))
         << 8U) |
        std::to_integer<std::uint8_t>(bytes[offset + 1U]));
}

[[nodiscard]] std::uint32_t read_u32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(bytes[offset]))
            << 24U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(bytes[offset + 1U]))
            << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(bytes[offset + 2U]))
            << 8U) |
           std::to_integer<std::uint8_t>(bytes[offset + 3U]);
}

[[nodiscard]] std::uint64_t read_u64(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    return (static_cast<std::uint64_t>(read_u32(bytes, offset)) << 32U) |
           read_u32(bytes, offset + 4U);
}

[[nodiscard]] std::int32_t read_i24(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    auto value = (static_cast<std::uint32_t>(
                      std::to_integer<std::uint8_t>(bytes[offset]))
                  << 16U) |
                 (static_cast<std::uint32_t>(
                      std::to_integer<std::uint8_t>(bytes[offset + 1U]))
                  << 8U) |
                 std::to_integer<std::uint8_t>(bytes[offset + 2U]);
    if ((value & 0x0080'0000U) != 0U) {
        value |= 0xff00'0000U;
    }
    return static_cast<std::int32_t>(value);
}

void append_u8(std::vector<std::byte>& bytes, const std::uint8_t value) {
    bytes.push_back(static_cast<std::byte>(value));
}

void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
    append_u8(bytes, static_cast<std::uint8_t>(value >> 8U));
    append_u8(bytes, static_cast<std::uint8_t>(value & 0xffU));
}

void append_u24(std::vector<std::byte>& bytes, const std::int32_t value) {
    const auto encoded = static_cast<std::uint32_t>(value);
    append_u8(bytes, static_cast<std::uint8_t>(encoded >> 16U));
    append_u8(bytes, static_cast<std::uint8_t>(encoded >> 8U));
    append_u8(bytes, static_cast<std::uint8_t>(encoded));
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    append_u8(bytes, static_cast<std::uint8_t>(value >> 24U));
    append_u8(bytes, static_cast<std::uint8_t>(value >> 16U));
    append_u8(bytes, static_cast<std::uint8_t>(value >> 8U));
    append_u8(bytes, static_cast<std::uint8_t>(value));
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
    append_u32(bytes, static_cast<std::uint32_t>(value >> 32U));
    append_u32(bytes, static_cast<std::uint32_t>(value));
}

[[nodiscard]] std::expected<void, std::string> validate_reports(
    const std::vector<ReceptionReportBlock>& reports) {
    if (reports.size() > maximum_count) {
        return std::unexpected{
            "an RTCP report packet cannot contain more than 31 blocks"};
    }
    for (const auto& report : reports) {
        if (report.cumulative_lost < minimum_signed_24 ||
            report.cumulative_lost > maximum_signed_24) {
            return std::unexpected{
                "RTCP cumulative loss must fit a signed 24-bit integer"};
        }
    }
    return {};
}

void append_header(std::vector<std::byte>& bytes,
                   const std::uint8_t count,
                   const std::uint8_t packet_type,
                   const std::size_t packet_size) {
    append_u8(bytes, static_cast<std::uint8_t>(
                         (protocol_version << 6U) | count));
    append_u8(bytes, packet_type);
    append_u16(bytes, static_cast<std::uint16_t>(packet_size / 4U - 1U));
}

void append_report(std::vector<std::byte>& bytes,
                   const ReceptionReportBlock& report) {
    append_u32(bytes, report.source_ssrc);
    append_u8(bytes, report.fraction_lost);
    append_u24(bytes, report.cumulative_lost);
    append_u32(bytes, report.extended_highest_sequence);
    append_u32(bytes, report.interarrival_jitter);
    append_u32(bytes, report.last_sender_report);
    append_u32(bytes, report.delay_since_last_sender_report);
}

[[nodiscard]] ReceptionReportBlock parse_report(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
    return {
        read_u32(bytes, offset),
        std::to_integer<std::uint8_t>(bytes[offset + 4U]),
        read_i24(bytes, offset + 5U),
        read_u32(bytes, offset + 8U),
        read_u32(bytes, offset + 12U),
        read_u32(bytes, offset + 16U),
        read_u32(bytes, offset + 20U),
    };
}

[[nodiscard]] SerializeResult serialize_sender_report(
    const SenderReport& report) {
    if (const auto valid = validate_reports(report.reports); !valid) {
        return std::unexpected{valid.error()};
    }
    const auto packet_size = sender_report_fixed_size +
                             report.reports.size() * reception_report_size;
    std::vector<std::byte> bytes;
    bytes.reserve(packet_size);
    append_header(bytes, static_cast<std::uint8_t>(report.reports.size()),
                  static_cast<std::uint8_t>(PacketType::SenderReport),
                  packet_size);
    append_u32(bytes, report.sender_ssrc);
    append_u64(bytes, report.ntp_timestamp);
    append_u32(bytes, report.rtp_timestamp);
    append_u32(bytes, report.sender_packet_count);
    append_u32(bytes, report.sender_octet_count);
    for (const auto& block : report.reports) {
        append_report(bytes, block);
    }
    return bytes;
}

[[nodiscard]] SerializeResult serialize_receiver_report(
    const ReceiverReport& report) {
    if (const auto valid = validate_reports(report.reports); !valid) {
        return std::unexpected{valid.error()};
    }
    const auto packet_size = receiver_report_fixed_size +
                             report.reports.size() * reception_report_size;
    std::vector<std::byte> bytes;
    bytes.reserve(packet_size);
    append_header(bytes, static_cast<std::uint8_t>(report.reports.size()),
                  static_cast<std::uint8_t>(PacketType::ReceiverReport),
                  packet_size);
    append_u32(bytes, report.sender_ssrc);
    for (const auto& block : report.reports) {
        append_report(bytes, block);
    }
    return bytes;
}

[[nodiscard]] SerializeResult serialize_source_description(
    const SourceDescription& description) {
    if (description.chunks.size() > maximum_count) {
        return std::unexpected{
            "an RTCP SDES packet cannot contain more than 31 chunks"};
    }

    std::vector<std::byte> body;
    for (const auto& chunk : description.chunks) {
        if (chunk.canonical_name.empty() ||
            chunk.canonical_name.size() >
                std::numeric_limits<std::uint8_t>::max()) {
            return std::unexpected{
                "an RTCP SDES CNAME must contain 1..255 bytes"};
        }
        append_u32(body, chunk.source_ssrc);
        append_u8(body, 1U);
        append_u8(body,
                  static_cast<std::uint8_t>(chunk.canonical_name.size()));
        for (const char character : chunk.canonical_name) {
            append_u8(body, static_cast<std::uint8_t>(character));
        }
        append_u8(body, 0U);
        while (body.size() % 4U != 0U) {
            append_u8(body, 0U);
        }
    }

    const auto packet_size = header_size + body.size();
    if (packet_size > maximum_packet_size) {
        return std::unexpected{"RTCP SDES packet is too large"};
    }
    std::vector<std::byte> bytes;
    bytes.reserve(packet_size);
    append_header(bytes,
                  static_cast<std::uint8_t>(description.chunks.size()),
                  static_cast<std::uint8_t>(PacketType::SourceDescription),
                  packet_size);
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

[[nodiscard]] SerializeResult serialize_unknown(
    const UnknownPacket& packet) {
    if (packet.count > maximum_count) {
        return std::unexpected{"RTCP count must be in 0..31"};
    }
    if (packet.body.size() % 4U != 0U) {
        return std::unexpected{
            "an unknown RTCP packet body must be 32-bit aligned"};
    }
    if (packet.packet_type ==
            static_cast<std::uint8_t>(PacketType::SenderReport) ||
        packet.packet_type ==
            static_cast<std::uint8_t>(PacketType::ReceiverReport) ||
        packet.packet_type ==
            static_cast<std::uint8_t>(PacketType::SourceDescription)) {
        return std::unexpected{
            "an unknown RTCP packet cannot use a supported packet type"};
    }
    const auto packet_size = header_size + packet.body.size();
    if (packet_size > maximum_packet_size) {
        return std::unexpected{"RTCP packet is too large"};
    }
    std::vector<std::byte> bytes;
    bytes.reserve(packet_size);
    append_header(bytes, packet.count, packet.packet_type, packet_size);
    bytes.insert(bytes.end(), packet.body.begin(), packet.body.end());
    return bytes;
}

[[nodiscard]] std::expected<SenderReport, ParseError> parse_sender_report(
    const std::span<const std::byte> bytes,
    const std::size_t packet_offset,
    const std::size_t body_end,
    const std::uint8_t report_count) {
    const auto expected_size = sender_report_fixed_size +
                               static_cast<std::size_t>(report_count) *
                                   reception_report_size;
    if (body_end - packet_offset != expected_size) {
        return std::unexpected{parse_error(
            ParseErrorCode::InvalidPacketBody, packet_offset,
            "RTCP sender report length does not match its report count")};
    }
    SenderReport report;
    report.sender_ssrc = read_u32(bytes, packet_offset + 4U);
    report.ntp_timestamp = read_u64(bytes, packet_offset + 8U);
    report.rtp_timestamp = read_u32(bytes, packet_offset + 16U);
    report.sender_packet_count = read_u32(bytes, packet_offset + 20U);
    report.sender_octet_count = read_u32(bytes, packet_offset + 24U);
    report.reports.reserve(report_count);
    auto cursor = packet_offset + sender_report_fixed_size;
    for (std::uint8_t index = 0; index < report_count; ++index) {
        report.reports.push_back(parse_report(bytes, cursor));
        cursor += reception_report_size;
    }
    return report;
}

[[nodiscard]] std::expected<ReceiverReport, ParseError>
parse_receiver_report(const std::span<const std::byte> bytes,
                      const std::size_t packet_offset,
                      const std::size_t body_end,
                      const std::uint8_t report_count) {
    const auto expected_size = receiver_report_fixed_size +
                               static_cast<std::size_t>(report_count) *
                                   reception_report_size;
    if (body_end - packet_offset != expected_size) {
        return std::unexpected{parse_error(
            ParseErrorCode::InvalidPacketBody, packet_offset,
            "RTCP receiver report length does not match its report count")};
    }
    ReceiverReport report;
    report.sender_ssrc = read_u32(bytes, packet_offset + 4U);
    report.reports.reserve(report_count);
    auto cursor = packet_offset + receiver_report_fixed_size;
    for (std::uint8_t index = 0; index < report_count; ++index) {
        report.reports.push_back(parse_report(bytes, cursor));
        cursor += reception_report_size;
    }
    return report;
}

[[nodiscard]] std::expected<SourceDescription, ParseError>
parse_source_description(const std::span<const std::byte> bytes,
                         const std::size_t packet_offset,
                         const std::size_t body_end,
                         const std::uint8_t chunk_count) {
    SourceDescription description;
    description.chunks.reserve(chunk_count);
    auto cursor = packet_offset + header_size;
    for (std::uint8_t chunk_index = 0; chunk_index < chunk_count;
         ++chunk_index) {
        const auto chunk_offset = cursor;
        if (body_end - cursor < 4U) {
            return std::unexpected{parse_error(
                ParseErrorCode::InvalidSourceDescription, cursor,
                "RTCP SDES chunk is missing its SSRC")};
        }
        SourceDescriptionChunk chunk;
        chunk.source_ssrc = read_u32(bytes, cursor);
        cursor += 4U;
        bool found_canonical_name = false;
        bool found_end = false;
        while (cursor < body_end) {
            const auto item_type =
                std::to_integer<std::uint8_t>(bytes[cursor++]);
            if (item_type == 0U) {
                found_end = true;
                break;
            }
            if (cursor >= body_end) {
                return std::unexpected{parse_error(
                    ParseErrorCode::InvalidSourceDescription, cursor,
                    "RTCP SDES item is missing its length")};
            }
            const auto item_size = static_cast<std::size_t>(
                std::to_integer<std::uint8_t>(bytes[cursor++]));
            if (item_size > body_end - cursor) {
                return std::unexpected{parse_error(
                    ParseErrorCode::InvalidSourceDescription, cursor,
                    "RTCP SDES item exceeds its packet boundary")};
            }
            if (item_type == 1U) {
                if (found_canonical_name || item_size == 0U) {
                    return std::unexpected{parse_error(
                        ParseErrorCode::InvalidSourceDescription, cursor,
                        "RTCP SDES chunk has an invalid CNAME")};
                }
                chunk.canonical_name.reserve(item_size);
                for (std::size_t index = 0; index < item_size; ++index) {
                    chunk.canonical_name.push_back(static_cast<char>(
                        std::to_integer<std::uint8_t>(bytes[cursor + index])));
                }
                found_canonical_name = true;
            }
            cursor += item_size;
        }
        if (!found_end || !found_canonical_name) {
            return std::unexpected{parse_error(
                ParseErrorCode::InvalidSourceDescription, chunk_offset,
                "RTCP SDES chunk must contain a CNAME and an end item")};
        }
        while ((cursor - chunk_offset) % 4U != 0U) {
            if (cursor >= body_end || bytes[cursor] != std::byte{0}) {
                return std::unexpected{parse_error(
                    ParseErrorCode::InvalidSourceDescription, cursor,
                    "RTCP SDES chunk has invalid alignment padding")};
            }
            ++cursor;
        }
        description.chunks.push_back(std::move(chunk));
    }
    if (cursor != body_end) {
        return std::unexpected{parse_error(
            ParseErrorCode::InvalidSourceDescription, cursor,
            "RTCP SDES packet contains trailing data")};
    }
    return description;
}

}  // namespace

ParseResult parse_compound_packet(const std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return std::unexpected{parse_error(
            ParseErrorCode::EmptyCompoundPacket, 0,
            "RTCP compound packet must not be empty")};
    }

    CompoundPacket compound;
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        if (bytes.size() - cursor < header_size) {
            return std::unexpected{parse_error(
                ParseErrorCode::TruncatedHeader, cursor,
                "RTCP packet is missing its complete common header")};
        }
        const auto first = std::to_integer<std::uint8_t>(bytes[cursor]);
        const auto version = static_cast<std::uint8_t>(first >> 6U);
        if (version != protocol_version) {
            return std::unexpected{parse_error(
                ParseErrorCode::UnsupportedVersion, cursor,
                "RTCP packet has an unsupported version")};
        }
        const bool has_padding = (first & 0x20U) != 0U;
        const auto count = static_cast<std::uint8_t>(first & 0x1fU);
        const auto packet_type =
            std::to_integer<std::uint8_t>(bytes[cursor + 1U]);
        const auto packet_size =
            (static_cast<std::size_t>(read_u16(bytes, cursor + 2U)) + 1U) *
            4U;
        if (packet_size < header_size || packet_size > bytes.size() - cursor) {
            return std::unexpected{parse_error(
                ParseErrorCode::InvalidLength, cursor,
                "RTCP packet length exceeds the compound packet boundary")};
        }
        const auto packet_end = cursor + packet_size;
        if (has_padding && packet_end != bytes.size()) {
            return std::unexpected{parse_error(
                ParseErrorCode::PaddingOnNonFinalPacket, cursor,
                "only the final RTCP packet may contain padding")};
        }
        auto body_end = packet_end;
        if (has_padding) {
            const auto padding_size = static_cast<std::size_t>(
                std::to_integer<std::uint8_t>(bytes[packet_end - 1U]));
            if (padding_size == 0U || padding_size > packet_size - header_size) {
                return std::unexpected{parse_error(
                    ParseErrorCode::InvalidPadding, packet_end - 1U,
                    "RTCP packet has an invalid padding length")};
            }
            body_end -= padding_size;
        }

        if (packet_type == static_cast<std::uint8_t>(PacketType::SenderReport)) {
            auto report =
                parse_sender_report(bytes, cursor, body_end, count);
            if (!report) {
                return std::unexpected{std::move(report.error())};
            }
            compound.packets.emplace_back(std::move(*report));
        } else if (packet_type ==
                   static_cast<std::uint8_t>(PacketType::ReceiverReport)) {
            auto report =
                parse_receiver_report(bytes, cursor, body_end, count);
            if (!report) {
                return std::unexpected{std::move(report.error())};
            }
            compound.packets.emplace_back(std::move(*report));
        } else if (packet_type ==
                   static_cast<std::uint8_t>(PacketType::SourceDescription)) {
            auto description =
                parse_source_description(bytes, cursor, body_end, count);
            if (!description) {
                return std::unexpected{std::move(description.error())};
            }
            compound.packets.emplace_back(std::move(*description));
        } else {
            UnknownPacket unknown;
            unknown.packet_type = packet_type;
            unknown.count = count;
            unknown.body.assign(bytes.begin() +
                                    static_cast<std::ptrdiff_t>(cursor +
                                                                header_size),
                                bytes.begin() +
                                    static_cast<std::ptrdiff_t>(body_end));
            compound.packets.emplace_back(std::move(unknown));
        }
        cursor = packet_end;
    }
    return compound;
}

SerializeResult serialize_compound_packet(const CompoundPacket& compound) {
    if (compound.packets.empty()) {
        return std::unexpected{"RTCP compound packet must not be empty"};
    }

    std::vector<std::byte> result;
    for (const auto& packet : compound.packets) {
        auto serialized = std::visit(
            [](const auto& value) -> SerializeResult {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, SenderReport>) {
                    return serialize_sender_report(value);
                } else if constexpr (std::is_same_v<Value, ReceiverReport>) {
                    return serialize_receiver_report(value);
                } else if constexpr (std::is_same_v<Value,
                                                    SourceDescription>) {
                    return serialize_source_description(value);
                } else {
                    return serialize_unknown(value);
                }
            },
            packet);
        if (!serialized) {
            return std::unexpected{std::move(serialized.error())};
        }
        result.insert(result.end(), serialized->begin(), serialized->end());
    }
    return result;
}

}  // namespace semilive::common::rtcp
