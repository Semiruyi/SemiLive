#include <semilive/common/rtcp/ntp_time.hpp>
#include <semilive/common/rtcp/rtcp_packet.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace rtcp = semilive::common::rtcp;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

[[nodiscard]] std::vector<std::byte> bytes(
    const std::initializer_list<std::uint8_t> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

void serializes_sender_report_and_sdes_as_network_bytes() {
    const rtcp::CompoundPacket compound{{
        rtcp::SenderReport{
            0x1122'3344U,
            0x0102'0304'0506'0708ULL,
            0x99aa'bbccU,
            2U,
            1'234U,
            {},
        },
        rtcp::SourceDescription{{
            {0x1122'3344U, "node"},
        }},
    }};

    const auto serialized = rtcp::serialize_compound_packet(compound);
    require(serialized.has_value(), "SR/SDES compound packet did not serialize");
    const auto expected = bytes({
        0x80, 0xc8, 0x00, 0x06,
        0x11, 0x22, 0x33, 0x44,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x99, 0xaa, 0xbb, 0xcc,
        0x00, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x04, 0xd2,
        0x81, 0xca, 0x00, 0x03,
        0x11, 0x22, 0x33, 0x44,
        0x01, 0x04, 0x6e, 0x6f, 0x64, 0x65, 0x00, 0x00,
    });
    require(*serialized == expected,
            "SR/SDES network representation is incorrect");

    const auto parsed = rtcp::parse_compound_packet(*serialized);
    require(parsed.has_value(), "serialized SR/SDES packet did not parse");
    require(*parsed == compound, "SR/SDES packet did not round trip");
}

void preserves_receiver_report_fields_and_signed_loss() {
    const auto encoded = bytes({
        0x81, 0xc9, 0x00, 0x07,
        0xaa, 0xbb, 0xcc, 0xdd,
        0x11, 0x22, 0x33, 0x44,
        0x40, 0xff, 0xff, 0xfe,
        0x00, 0x01, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x20,
        0x03, 0x04, 0x05, 0x06,
        0x00, 0x00, 0x80, 0x00,
    });
    const auto parsed = rtcp::parse_compound_packet(encoded);
    require(parsed.has_value(), "receiver report did not parse");
    require(parsed->packets.size() == 1,
            "receiver report produced the wrong packet count");
    const auto* report =
        std::get_if<rtcp::ReceiverReport>(&parsed->packets.front());
    require(report != nullptr && report->sender_ssrc == 0xaabb'ccddU,
            "receiver report sender SSRC was not preserved");
    require(report->reports.size() == 1,
            "receiver report block was not parsed");
    const auto& block = report->reports.front();
    require(block.source_ssrc == 0x1122'3344U &&
                block.fraction_lost == 0x40U &&
                block.cumulative_lost == -2 &&
                block.extended_highest_sequence == 0x0001'0002U &&
                block.interarrival_jitter == 0x20U &&
                block.last_sender_report == 0x0304'0506U &&
                block.delay_since_last_sender_report == 0x0000'8000U,
            "receiver report block fields were not preserved");

    const auto serialized = rtcp::serialize_compound_packet(*parsed);
    require(serialized.has_value() && *serialized == encoded,
            "receiver report did not serialize back to its source bytes");
}

void skips_unknown_packet_types_without_losing_boundaries() {
    const auto encoded = bytes({
        0x83, 0xcf, 0x00, 0x01,
        0xde, 0xad, 0xbe, 0xef,
        0x80, 0xc9, 0x00, 0x01,
        0x01, 0x02, 0x03, 0x04,
    });
    const auto parsed = rtcp::parse_compound_packet(encoded);
    require(parsed.has_value() && parsed->packets.size() == 2,
            "unknown RTCP packet broke compound traversal");
    const auto* unknown =
        std::get_if<rtcp::UnknownPacket>(&parsed->packets.front());
    require(unknown != nullptr && unknown->packet_type == 207U &&
                unknown->count == 3U &&
                unknown->body == bytes({0xde, 0xad, 0xbe, 0xef}),
            "unknown RTCP packet was not preserved");
    require(std::holds_alternative<rtcp::ReceiverReport>(
                parsed->packets.back()),
            "packet after unknown RTCP data was not parsed");
}

void serializes_and_parses_generic_nack_network_bytes() {
    const rtcp::GenericNack nack{
        0x1122'3344U,
        0x5566'7788U,
        {{1'000U, 0x0005U}, {2'000U, 0x8000U}},
    };
    const auto serialized = rtcp::serialize_compound_packet({{nack}});
    require(serialized.has_value(), "Generic NACK did not serialize");
    const auto expected = bytes({
        0x81, 0xcd, 0x00, 0x04,
        0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88,
        0x03, 0xe8, 0x00, 0x05,
        0x07, 0xd0, 0x80, 0x00,
    });
    require(*serialized == expected,
            "Generic NACK network representation is incorrect");

    const auto parsed = rtcp::parse_compound_packet(expected);
    require(parsed.has_value() && parsed->packets.size() == 1,
            "Generic NACK did not parse");
    const auto* parsed_nack =
        std::get_if<rtcp::GenericNack>(&parsed->packets.front());
    require(parsed_nack != nullptr && *parsed_nack == nack,
            "Generic NACK fields were not preserved");
}

void packs_generic_nack_sequences_across_wraparound() {
    const std::vector<std::uint16_t> lost{
        65'534U, 65'535U, 0U, 2U, 2U, 20U,
    };
    const auto blocks = rtcp::pack_generic_nack_blocks(lost);
    require(blocks == std::vector<rtcp::GenericNackBlock>{
                          {65'534U, 0x000bU}, {20U, 0U}},
            "lost sequences were not packed into PID/BLP blocks");
    require(rtcp::expand_generic_nack_blocks(blocks) ==
                std::vector<std::uint16_t>{65'534U, 65'535U, 0U, 2U, 20U},
            "PID/BLP blocks did not expand across sequence wraparound");
}

void validates_headers_lengths_padding_and_report_ranges() {
    const auto wrong_version =
        rtcp::parse_compound_packet(bytes({0x40, 0xc9, 0x00, 0x01,
                                          0x00, 0x00, 0x00, 0x01}));
    require(!wrong_version &&
                wrong_version.error().code ==
                    rtcp::ParseErrorCode::UnsupportedVersion,
            "unsupported RTCP version was accepted");

    const auto truncated =
        rtcp::parse_compound_packet(bytes({0x80, 0xc9, 0x00, 0x02,
                                          0x00, 0x00, 0x00, 0x01}));
    require(!truncated &&
                truncated.error().code == rtcp::ParseErrorCode::InvalidLength,
            "RTCP packet exceeding its datagram was accepted");

    const auto non_final_padding = rtcp::parse_compound_packet(bytes({
        0xa0, 0xcf, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04,
        0x80, 0xc9, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    }));
    require(!non_final_padding &&
                non_final_padding.error().code ==
                    rtcp::ParseErrorCode::PaddingOnNonFinalPacket,
            "padding on a non-final RTCP packet was accepted");

    rtcp::ReceiverReport out_of_range;
    out_of_range.reports.push_back(
        rtcp::ReceptionReportBlock{.cumulative_lost = 8'388'608});
    const auto invalid_loss =
        rtcp::serialize_compound_packet({{out_of_range}});
    require(!invalid_loss,
            "cumulative loss outside signed 24-bit range was accepted");

    const rtcp::UnknownPacket disguised_sender_report{
        static_cast<std::uint8_t>(rtcp::PacketType::SenderReport), 0, {}};
    require(!rtcp::serialize_compound_packet({{disguised_sender_report}}),
            "unknown packet accepted a supported RTCP packet type");

    const auto empty_nack =
        rtcp::serialize_compound_packet({{rtcp::GenericNack{}}});
    require(!empty_nack, "Generic NACK without feedback was accepted");

    const auto truncated_nack = rtcp::parse_compound_packet(bytes({
        0x81, 0xcd, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x02,
    }));
    require(!truncated_nack &&
                truncated_nack.error().code ==
                    rtcp::ParseErrorCode::InvalidPacketBody,
            "Generic NACK without an FCI block was accepted");

    const rtcp::UnknownPacket other_feedback_format{
        static_cast<std::uint8_t>(rtcp::PacketType::TransportLayerFeedback),
        2U,
        bytes({0x00, 0x00, 0x00, 0x01}),
    };
    require(rtcp::serialize_compound_packet({{other_feedback_format}})
                .has_value(),
            "unsupported transport feedback format was not preserved as unknown");
}

void converts_ntp_timestamps_and_compact_durations() {
    const auto unix_epoch = rtcp::ntp_timestamp(
        std::chrono::system_clock::time_point{});
    require(unix_epoch.has_value() &&
                *unix_epoch == 0x83aa'7e80'0000'0000ULL,
            "Unix epoch did not convert to the expected NTP timestamp");

    const auto half_second = rtcp::ntp_timestamp(
        std::chrono::system_clock::time_point{} + 500ms);
    require(half_second.has_value() &&
                *half_second == 0x83aa'7e80'8000'0000ULL,
            "NTP fractional timestamp conversion is incorrect");
    require(rtcp::compact_ntp(*half_second) == 0x7e80'8000U,
            "compact NTP extraction is incorrect");

    const auto compact_duration = rtcp::compact_ntp_duration(1500ms);
    require(compact_duration.has_value() &&
                *compact_duration == 0x0001'8000U,
            "compact NTP duration encoding is incorrect");
    require(rtcp::duration_from_compact_ntp(*compact_duration) == 1500ms,
            "compact NTP duration did not round trip");
    require(!rtcp::compact_ntp_duration(-1ns),
            "negative compact NTP duration was accepted");
}

}  // namespace

int main() {
    try {
        serializes_sender_report_and_sdes_as_network_bytes();
        preserves_receiver_report_fields_and_signed_loss();
        skips_unknown_packet_types_without_losing_boundaries();
        serializes_and_parses_generic_nack_network_bytes();
        packs_generic_nack_sequences_across_wraparound();
        validates_headers_lengths_padding_and_report_ranges();
        converts_ntp_timestamps_and_compact_durations();
        std::cout << "RTCP protocol tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "RTCP protocol test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
