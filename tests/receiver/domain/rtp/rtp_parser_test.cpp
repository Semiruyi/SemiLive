#include <semilive/receiver/domain/rtp/rtp_parser.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace domain = semilive::receiver::domain;
namespace model = semilive::receiver::model;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

[[nodiscard]] std::vector<std::byte> bytes(
    const std::initializer_list<std::uint8_t> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    std::ranges::transform(values, std::back_inserter(result),
                           [](const std::uint8_t value) {
                               return static_cast<std::byte>(value);
                           });
    return result;
}

[[nodiscard]] std::vector<std::byte> base_packet() {
    return bytes({
        0x80,
        0x60,
        0x12,
        0x34,
        0x01,
        0x23,
        0x45,
        0x67,
        0x89,
        0xAB,
        0xCD,
        0xEF,
    });
}

[[nodiscard]] model::UdpDatagram datagram(
    std::vector<std::byte> packet,
    const model::UdpDatagram::Clock::time_point received_at = {}) {
    return {std::move(packet), received_at};
}

void require_error(const domain::RtpParseResult& result,
                   const domain::RtpParseErrorCode expected,
                   const std::string_view message) {
    require(!result, message);
    require(result.error().code == expected,
            "RTP parse failure reported the wrong error code");
    require(!result.error().message.empty(),
            "RTP parse failure must include a diagnostic message");
}

void parses_fixed_header_and_preserves_arrival_time() {
    auto packet_bytes = base_packet();
    const auto payload = bytes({0x65, 0xAA, 0xBB});
    packet_bytes.insert(packet_bytes.end(), payload.begin(), payload.end());
    packet_bytes[1] = static_cast<std::byte>(0xE0);
    const auto received_at =
        model::UdpDatagram::Clock::time_point{std::chrono::milliseconds{42}};

    const auto result =
        domain::RtpParser{}.parse(datagram(std::move(packet_bytes),
                                           received_at));
    require(result.has_value(), "valid RTP packet must parse");
    require(result->marker(), "marker bit must be parsed separately");
    require(result->payload_type() == 96,
            "payload type must exclude the marker bit");
    require(result->sequence_number() == 0x1234,
            "sequence number must use network byte order");
    require(result->timestamp() == 0x01234567,
            "timestamp must use network byte order");
    require(result->ssrc() == 0x89ABCDEF,
            "SSRC must use network byte order");
    require(result->received_at() == received_at,
            "arrival time must survive parsing");
    require(std::ranges::equal(result->payload(), payload),
            "payload bytes must exclude the RTP header");
    require(result->datagram().size() == 15,
            "parsed RTP packet must own the original datagram");
}

void accepts_an_empty_payload() {
    const auto result = domain::RtpParser{}.parse(datagram(base_packet()));
    require(result.has_value(),
            "generic RTP parser must accept an empty payload");
    require(result->payload().empty(), "empty RTP payload must remain empty");
}

void skips_csrc_entries() {
    auto packet = base_packet();
    packet[0] = static_cast<std::byte>(0x82);
    const auto suffix = bytes({
        0x10, 0x20, 0x30, 0x40,
        0x50, 0x60, 0x70, 0x80,
        0x61, 0x01,
    });
    packet.insert(packet.end(), suffix.begin(), suffix.end());

    const auto result = domain::RtpParser{}.parse(datagram(std::move(packet)));
    require(result.has_value(), "RTP packet with CSRC entries must parse");
    require(std::ranges::equal(result->payload(), bytes({0x61, 0x01})),
            "CSRC entries must not appear in the payload");
}

void skips_header_extension() {
    auto packet = base_packet();
    packet[0] = static_cast<std::byte>(0x90);
    const auto suffix = bytes({
        0xBE, 0xDE, 0x00, 0x01,
        0x11, 0x22, 0x33, 0x44,
        0x41, 0x02,
    });
    packet.insert(packet.end(), suffix.begin(), suffix.end());

    const auto result = domain::RtpParser{}.parse(datagram(std::move(packet)));
    require(result.has_value(),
            "RTP packet with a header extension must parse");
    require(std::ranges::equal(result->payload(), bytes({0x41, 0x02})),
            "extension bytes must not appear in the payload");
}

void removes_valid_padding() {
    auto packet = base_packet();
    packet[0] = static_cast<std::byte>(0xA0);
    const auto suffix = bytes({0x65, 0x00, 0x00, 0x03});
    packet.insert(packet.end(), suffix.begin(), suffix.end());

    const auto result = domain::RtpParser{}.parse(datagram(std::move(packet)));
    require(result.has_value(), "padded RTP packet must parse");
    require(std::ranges::equal(result->payload(), bytes({0x65})),
            "padding bytes must not appear in the payload");
}

void rejects_short_and_non_v2_packets() {
    require_error(domain::RtpParser{}.parse(datagram(bytes({0x80, 0x60}))),
                  domain::RtpParseErrorCode::DatagramTooShort,
                  "short fixed header must fail");

    auto wrong_version = base_packet();
    wrong_version[0] = static_cast<std::byte>(0x40);
    require_error(domain::RtpParser{}.parse(
                      datagram(std::move(wrong_version))),
                  domain::RtpParseErrorCode::UnsupportedVersion,
                  "non-v2 RTP packet must fail");
}

void rejects_truncated_csrc_and_extensions() {
    auto csrc = base_packet();
    csrc[0] = static_cast<std::byte>(0x81);
    require_error(domain::RtpParser{}.parse(datagram(std::move(csrc))),
                  domain::RtpParseErrorCode::TruncatedCsrcList,
                  "truncated CSRC list must fail");

    auto extension_header = base_packet();
    extension_header[0] = static_cast<std::byte>(0x90);
    extension_header.push_back(static_cast<std::byte>(0xBE));
    require_error(
        domain::RtpParser{}.parse(datagram(std::move(extension_header))),
        domain::RtpParseErrorCode::TruncatedExtensionHeader,
        "truncated extension header must fail");

    auto extension_data = base_packet();
    extension_data[0] = static_cast<std::byte>(0x90);
    const auto incomplete_extension =
        bytes({0xBE, 0xDE, 0x00, 0x02, 0x11, 0x22, 0x33, 0x44});
    extension_data.insert(extension_data.end(), incomplete_extension.begin(),
                          incomplete_extension.end());
    require_error(
        domain::RtpParser{}.parse(datagram(std::move(extension_data))),
        domain::RtpParseErrorCode::TruncatedExtensionData,
        "truncated extension data must fail");
}

void rejects_zero_and_oversized_padding() {
    auto zero_padding = base_packet();
    zero_padding[0] = static_cast<std::byte>(0xA0);
    zero_padding.push_back(static_cast<std::byte>(0x00));
    require_error(domain::RtpParser{}.parse(
                      datagram(std::move(zero_padding))),
                  domain::RtpParseErrorCode::InvalidPadding,
                  "zero RTP padding length must fail");

    auto oversized_padding = base_packet();
    oversized_padding[0] = static_cast<std::byte>(0xA0);
    oversized_padding.push_back(static_cast<std::byte>(0x02));
    require_error(domain::RtpParser{}.parse(
                      datagram(std::move(oversized_padding))),
                  domain::RtpParseErrorCode::InvalidPadding,
                  "padding larger than remaining bytes must fail");
}

}  // namespace

int main() {
    try {
        parses_fixed_header_and_preserves_arrival_time();
        accepts_an_empty_payload();
        skips_csrc_entries();
        skips_header_extension();
        removes_valid_padding();
        rejects_short_and_non_v2_packets();
        rejects_truncated_csrc_and_extensions();
        rejects_zero_and_oversized_padding();
    } catch (const std::exception& error) {
        std::cerr << "RTP parser test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
