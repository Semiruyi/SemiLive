#include "annex_b_nal_splitter.hpp"
#include "h264_rtp_packetizer.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace detail = semilive::publisher::infra::output::detail;

using Bytes = std::vector<std::byte>;
using Datagrams = std::vector<Bytes>;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

[[nodiscard]] std::uint8_t value(const std::byte byte) noexcept {
    return std::to_integer<std::uint8_t>(byte);
}

[[nodiscard]] std::uint16_t read_u16(const Bytes& bytes,
                                     const std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(value(bytes.at(offset))) << 8U) |
        value(bytes.at(offset + 1)));
}

[[nodiscard]] std::uint32_t read_u32(const Bytes& bytes,
                                     const std::size_t offset) {
    return (static_cast<std::uint32_t>(value(bytes.at(offset))) << 24U) |
           (static_cast<std::uint32_t>(value(bytes.at(offset + 1))) << 16U) |
           (static_cast<std::uint32_t>(value(bytes.at(offset + 2))) << 8U) |
           value(bytes.at(offset + 3));
}

[[nodiscard]] detail::RtpDatagramEmitter collecting_emitter(
    Datagrams& datagrams) {
    return [&datagrams](const std::span<const std::byte> datagram)
               -> detail::RtpDatagramEmitResult {
        datagrams.emplace_back(datagram.begin(), datagram.end());
        return {};
    };
}

void splits_mixed_annex_b_start_codes_without_copying_payloads() {
    const Bytes annex_b{
        std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x67}, std::byte{0x11}, std::byte{0x80},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x65}, std::byte{0x22}, std::byte{0x80},
        std::byte{0x00}, std::byte{0x00},
    };

    const auto result = detail::split_annex_b_nal_units(annex_b);
    require(result.has_value(), "mixed Annex-B start codes must split");
    require(result->size() == 2, "splitter must preserve both NAL units");
    require((*result)[0].data() == annex_b.data() + 6 &&
                (*result)[0].size() == 3,
            "first NAL must be a view after the four-byte start code");
    require((*result)[1].data() == annex_b.data() + 12 &&
                (*result)[1].size() == 3,
            "second NAL must exclude start code and trailing zero bytes");
}

void rejects_invalid_annex_b_and_nal_headers() {
    const std::array invalid_inputs{
        Bytes{},
        Bytes{std::byte{0x65}, std::byte{0x80}},
        Bytes{std::byte{0x12}, std::byte{0x00}, std::byte{0x00},
              std::byte{0x01}, std::byte{0x65}, std::byte{0x80}},
        Bytes{std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
              std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
              std::byte{0x65}, std::byte{0x80}},
        Bytes{std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
              std::byte{0x65}},
        Bytes{std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
              std::byte{0xe5}, std::byte{0x80}},
        Bytes{std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
              std::byte{0x60}, std::byte{0x80}},
        Bytes{std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
              std::byte{0x78}, std::byte{0x80}},
    };

    for (const auto& input : invalid_inputs) {
        require(!detail::split_annex_b_nal_units(input),
                "invalid Annex-B input must be rejected");
    }
}

void emits_single_nal_packets_with_one_marker_per_access_unit() {
    const Bytes annex_b{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x67}, std::byte{0x11}, std::byte{0x80},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x65}, std::byte{0x22}, std::byte{0x80},
    };
    detail::H264RtpPacketizer packetizer{{97, 20}};
    detail::RtpSessionState session{65'535, 0x10203040U, 0xa1b2c3d4U};
    Datagrams datagrams;

    const auto result = packetizer.packetize(
        annex_b, std::chrono::milliseconds{3500}, session,
        collecting_emitter(datagrams));

    require(result && result->emitted_datagrams == 2 &&
                result->emitted_bytes == 30,
            "two small NAL units must produce two RTP datagrams");
    require(datagrams.size() == 2, "emitter must observe both datagrams");
    require(value(datagrams[0][0]) == 0x80 &&
                value(datagrams[0][1]) == 97,
            "first RTP header must use V=2, M=0 and configured PT");
    require(value(datagrams[1][1]) == 0xe1,
            "only the final packet of the access unit must set Marker");
    require(read_u16(datagrams[0], 2) == 65'535 &&
                read_u16(datagrams[1], 2) == 0 && session.next_sequence == 1,
            "RTP sequence must advance continuously with 16-bit wraparound");
    require(read_u32(datagrams[0], 4) == 0x1024feb8U &&
                read_u32(datagrams[1], 4) == 0x1024feb8U,
            "all packets in one AU must share its rounded 90 kHz timestamp");
    require(read_u32(datagrams[0], 8) == 0xa1b2c3d4U,
            "RTP header must contain the session SSRC in network order");
    require(Bytes{datagrams[0].begin() + 12, datagrams[0].end()} ==
                Bytes{std::byte{0x67}, std::byte{0x11}, std::byte{0x80}},
            "single-NAL RTP payload must be the complete original NAL");
}

void fragments_large_nal_as_fu_a_and_reconstructs_original_bytes() {
    const Bytes annex_b{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x65}, std::byte{0x10}, std::byte{0x11}, std::byte{0x12},
        std::byte{0x13}, std::byte{0x14}, std::byte{0x15}, std::byte{0x16},
        std::byte{0x17}, std::byte{0x80},
    };
    detail::H264RtpPacketizer packetizer{{96, 18}};
    detail::RtpSessionState session{7, 1234, 4321};
    Datagrams datagrams;

    const auto result = packetizer.packetize(
        annex_b, std::chrono::nanoseconds{0}, session,
        collecting_emitter(datagrams));

    require(result && result->emitted_datagrams == 3 &&
                result->emitted_bytes == 51,
            "large NAL must split into bounded FU-A datagrams");
    require(datagrams.size() == 3 && datagrams[0].size() == 18 &&
                datagrams[1].size() == 18 && datagrams[2].size() == 15,
            "FU-A fragments must honor maximum datagram size");
    require(value(datagrams[0][12]) == 0x7c &&
                value(datagrams[1][12]) == 0x7c &&
                value(datagrams[2][12]) == 0x7c,
            "FU indicator must preserve F/NRI and select type 28");
    require(value(datagrams[0][13]) == 0x85 &&
                value(datagrams[1][13]) == 0x05 &&
                value(datagrams[2][13]) == 0x45,
            "FU headers must mark only the first and final fragments");
    require((value(datagrams[0][1]) & 0x80U) == 0 &&
                (value(datagrams[1][1]) & 0x80U) == 0 &&
                (value(datagrams[2][1]) & 0x80U) != 0,
            "only final FU-A fragment of the final NAL must set Marker");

    Bytes reconstructed{std::byte{0x65}};
    for (const auto& datagram : datagrams) {
        reconstructed.insert(reconstructed.end(), datagram.begin() + 14,
                             datagram.end());
    }
    require(reconstructed ==
                Bytes{annex_b.begin() + 3, annex_b.end()},
            "FU-A fragments must reconstruct the exact original NAL");
}

void rounds_timestamps_and_propagates_emitter_failure() {
    const Bytes annex_b{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x61}, std::byte{0x80},
    };
    detail::H264RtpPacketizer packetizer{{96, 1200}};
    detail::RtpSessionState below_half_tick{1, 10, 20};
    detail::RtpSessionState at_half_tick{1, 10, 20};
    Datagrams below;
    Datagrams above;

    require(packetizer
                .packetize(annex_b, std::chrono::nanoseconds{5555},
                           below_half_tick, collecting_emitter(below))
                .has_value(),
            "timestamp below half a clock tick must packetize");
    require(packetizer
                .packetize(annex_b, std::chrono::nanoseconds{5556},
                           at_half_tick, collecting_emitter(above))
                .has_value(),
            "timestamp above half a clock tick must packetize");
    require(read_u32(below[0], 4) == 10 && read_u32(above[0], 4) == 11,
            "90 kHz timestamp conversion must round to nearest tick");

    detail::RtpSessionState wrapping_timestamp{1, 0xfffffff0U, 20};
    Datagrams wrapped;
    require(packetizer
                .packetize(annex_b, std::chrono::milliseconds{1},
                           wrapping_timestamp, collecting_emitter(wrapped))
                .has_value(),
            "timestamp wraparound input must packetize");
    require(read_u32(wrapped[0], 4) == 74,
            "RTP timestamp must wrap naturally at 32 bits");

    detail::RtpSessionState failed_session{42, 0, 0};
    const auto failure = packetizer.packetize(
        annex_b, std::chrono::nanoseconds{0}, failed_session,
        [](std::span<const std::byte>) -> detail::RtpDatagramEmitResult {
            return std::unexpected{"synthetic send failure"};
        });
    require(!failure && failure.error() == "synthetic send failure" &&
                failed_session.next_sequence == 43,
            "emitter failure must propagate after consuming its sequence");
}

void rejects_invalid_packetizer_inputs_without_emitting() {
    const Bytes valid_annex_b{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x61}, std::byte{0x80},
    };
    Datagrams datagrams;
    detail::RtpSessionState session{};
    detail::H264RtpPacketizer invalid_pt{{95, 1200}};
    detail::H264RtpPacketizer too_small{{96, 14}};
    detail::H264RtpPacketizer too_large{{96, 65'508}};

    require(!invalid_pt.packetize(valid_annex_b, std::chrono::nanoseconds{0},
                                  session, collecting_emitter(datagrams)),
            "static RTP payload types must be rejected");
    require(!too_small.packetize(valid_annex_b, std::chrono::nanoseconds{0},
                                 session, collecting_emitter(datagrams)),
            "datagrams too small for FU-A must be rejected");
    require(!too_large.packetize(valid_annex_b, std::chrono::nanoseconds{0},
                                 session, collecting_emitter(datagrams)),
            "datagrams above UDP payload maximum must be rejected");

    detail::H264RtpPacketizer valid_packetizer{{96, 1200}};
    require(!valid_packetizer.packetize(
                valid_annex_b, std::chrono::nanoseconds{-1}, session,
                collecting_emitter(datagrams)),
            "negative presentation timestamps must be rejected");
    require(datagrams.empty(), "invalid inputs must not emit RTP datagrams");
}

}  // namespace

int main() {
    try {
        splits_mixed_annex_b_start_codes_without_copying_payloads();
        rejects_invalid_annex_b_and_nal_headers();
        emits_single_nal_packets_with_one_marker_per_access_unit();
        fragments_large_nal_as_fu_a_and_reconstructs_original_bytes();
        rounds_timestamps_and_propagates_emitter_failure();
        rejects_invalid_packetizer_inputs_without_emitting();
    } catch (const std::exception& error) {
        std::cerr << "H.264 RTP packetizer test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "H.264 RTP packetizer tests passed\n";
    return EXIT_SUCCESS;
}
