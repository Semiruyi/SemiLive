#include <semilive/receiver/domain/h264/h264_rtp_depacketizer.hpp>

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
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace domain = semilive::receiver::domain;
namespace model = semilive::receiver::model;

using Clock = model::RtpPacket::Clock;

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

[[nodiscard]] domain::RtpReorderEvent packet(
    const std::uint16_t sequence,
    std::vector<std::byte> payload,
    const bool marker = false,
    const std::uint32_t timestamp = 90'000,
    const std::chrono::milliseconds completed_at = 0ms) {
    const auto payload_size = payload.size();
    return domain::OrderedRtpPacket{model::RtpPacket{
        std::move(payload), Clock::time_point{completed_at}, marker, 96,
        sequence, timestamp, 0x12345678, 0, payload_size}};
}

[[nodiscard]] const model::H264NalUnit& nal(
    const domain::H264DepacketizerEvent& event,
    const std::string_view message) {
    const auto* value = std::get_if<model::H264NalUnit>(&event);
    require(value != nullptr, message);
    return *value;
}

[[nodiscard]] const domain::H264DepacketizationDiscontinuity& discontinuity(
    const domain::H264DepacketizerEvent& event,
    const std::string_view message) {
    const auto* value =
        std::get_if<domain::H264DepacketizationDiscontinuity>(&event);
    require(value != nullptr, message);
    return *value;
}

void emits_single_nal_units_with_rtp_metadata() {
    domain::H264RtpDepacketizer depacketizer;
    const auto events = depacketizer.consume(
        packet(10, bytes({0x65, 0xAA, 0xBB}), true, 1234, 7ms));

    require(events.size() == 1, "single NAL packet must emit one event");
    const auto& value = nal(events[0], "single NAL event must contain a NAL");
    require(std::ranges::equal(value.bytes(), bytes({0x65, 0xAA, 0xBB})),
            "single NAL bytes must be preserved without an Annex-B prefix");
    require(value.nal_unit_type() == 5 && value.rtp_timestamp() == 1234 &&
                value.marker(),
            "single NAL RTP metadata must be preserved");
    require(value.first_sequence() == 10 && value.last_sequence() == 10,
            "single NAL sequence range must contain its packet");
    require(value.completed_at() == Clock::time_point{7ms},
            "single NAL completion time must be preserved");

    const auto stats = depacketizer.stats();
    require(stats.completed_nal_units == 1 && stats.single_nal_units == 1,
            "single NAL completion must be counted");
}

void reconstructs_a_multi_packet_fu_a() {
    domain::H264RtpDepacketizer depacketizer;
    require(depacketizer.consume(
                packet(20, bytes({0x7C, 0x85, 0xAA, 0xBB})))
                .empty(),
            "FU-A start must remain pending");
    require(depacketizer.consume(
                packet(21, bytes({0x7C, 0x05, 0xCC})))
                .empty(),
            "FU-A middle must remain pending");

    const auto events = depacketizer.consume(
        packet(22, bytes({0x7C, 0x45, 0xDD}), true, 90'000, 9ms));
    require(events.size() == 1, "FU-A end must emit one complete NAL");
    const auto& value = nal(events[0], "FU-A output must contain a NAL");
    require(std::ranges::equal(
                value.bytes(), bytes({0x65, 0xAA, 0xBB, 0xCC, 0xDD})),
            "FU-A must reconstruct the original header and payload");
    require(value.first_sequence() == 20 && value.last_sequence() == 22 &&
                value.marker(),
            "FU-A output must describe its packet range and final marker");
    require(value.completed_at() == Clock::time_point{9ms},
            "FU-A must use the final fragment completion time");
    require(depacketizer.stats().fu_a_nal_units == 1 &&
                depacketizer.stats().pending_fragment_bytes == 0,
            "completed FU-A must clear pending state and update statistics");
}

void reconstructs_fu_a_across_sequence_wraparound() {
    domain::H264RtpDepacketizer depacketizer;
    static_cast<void>(depacketizer.consume(
        packet(65'535, bytes({0x7C, 0x81, 0xAA}))));

    const auto completed = depacketizer.consume(
        packet(0, bytes({0x7C, 0x41, 0xBB}), true));
    require(completed.size() == 1 &&
                std::ranges::equal(
                    nal(completed[0], "wrapped FU-A must complete").bytes(),
                    bytes({0x61, 0xAA, 0xBB})),
            "FU-A sequence continuity must use 16-bit wraparound");
}

void sequence_gap_abandons_an_incomplete_fu_a() {
    domain::H264RtpDepacketizer depacketizer;
    static_cast<void>(depacketizer.consume(
        packet(30, bytes({0x7C, 0x81, 0xAA}))));

    const auto events = depacketizer.consume(domain::RtpSequenceGap{
        31, 33, 2, domain::RtpSequenceGapCause::HoldTimeout});
    require(events.size() == 1, "sequence gap must emit one discontinuity");
    const auto& value = discontinuity(
        events[0], "sequence gap output must be a discontinuity");
    require(value.code ==
                domain::H264DepacketizationDiscontinuityCode::SequenceGap &&
                value.related_sequence == 31 && value.missing_packets == 2,
            "sequence gap metadata must reach the depacketizer output");
    require(depacketizer.stats().pending_fragment_bytes == 0 &&
                depacketizer.stats().abandoned_fragmented_nal_units == 1,
            "sequence gap must discard the incomplete FU-A");

    const auto orphan = depacketizer.consume(
        packet(33, bytes({0x7C, 0x41, 0xBB}), true));
    require(discontinuity(orphan[0], "post-gap FU-A end must be orphaned")
                .code == domain::H264DepacketizationDiscontinuityCode::
                             OrphanFuAFragment,
            "FU-A continuation after a gap must not form a NAL");
}

void valid_packet_interrupts_an_unfinished_fu_a_then_continues() {
    domain::H264RtpDepacketizer depacketizer;
    static_cast<void>(depacketizer.consume(
        packet(40, bytes({0x7C, 0x81, 0xAA}))));

    const auto single = depacketizer.consume(
        packet(41, bytes({0x61, 0x11}), true));
    require(single.size() == 2,
            "single NAL must report interruption before its output");
    require(discontinuity(single[0], "first event must report interruption")
                .code == domain::H264DepacketizationDiscontinuityCode::
                             FragmentInterrupted,
            "unfinished FU-A must be reported as interrupted");
    require(nal(single[1], "valid single NAL must still be emitted")
                .nal_unit_type() == 1,
            "valid packet after interruption must not be discarded");

    static_cast<void>(depacketizer.consume(
        packet(42, bytes({0x7C, 0x81, 0x22}))));
    const auto new_start = depacketizer.consume(
        packet(43, bytes({0x7C, 0x85, 0x33})));
    require(new_start.size() == 1 &&
                discontinuity(new_start[0],
                              "new FU-A start must report interruption")
                        .code == domain::
                                     H264DepacketizationDiscontinuityCode::
                                         FragmentInterrupted,
            "new FU-A start must replace the unfinished fragmented NAL");
    const auto completed = depacketizer.consume(
        packet(44, bytes({0x7C, 0x45, 0x44}), true));
    require(completed.size() == 1 &&
                std::ranges::equal(
                    nal(completed[0], "replacement FU-A must complete").bytes(),
                    bytes({0x65, 0x33, 0x44})),
            "replacement FU-A must start from its own payload");
}

void rejects_malformed_and_unsupported_packets() {
    domain::H264RtpDepacketizer depacketizer;
    const auto check = [&depacketizer](
                           const std::uint16_t sequence,
                           std::vector<std::byte> payload,
                           const domain::H264DepacketizationDiscontinuityCode
                               expected) {
        const auto events =
            depacketizer.consume(packet(sequence, std::move(payload)));
        require(events.size() == 1,
                "invalid H.264 RTP packet must emit one discontinuity");
        require(discontinuity(events[0],
                              "invalid packet must emit a discontinuity")
                    .code == expected,
                "invalid packet reported the wrong discontinuity code");
    };

    check(50, {},
          domain::H264DepacketizationDiscontinuityCode::EmptyPayload);
    check(51, bytes({0xE5, 0x01}),
          domain::H264DepacketizationDiscontinuityCode::ForbiddenBitSet);
    check(52, bytes({0x78, 0x01}),
          domain::H264DepacketizationDiscontinuityCode::
              UnsupportedPacketizationMode);
    check(53, bytes({0x7C, 0x85}),
          domain::H264DepacketizationDiscontinuityCode::MalformedFuA);
    check(54, bytes({0x7C, 0xC5, 0x01}),
          domain::H264DepacketizationDiscontinuityCode::MalformedFuA);
    check(55, bytes({0x7C, 0x25, 0x01}),
          domain::H264DepacketizationDiscontinuityCode::MalformedFuA);
    check(56, bytes({0x7C, 0x05, 0x01}),
          domain::H264DepacketizationDiscontinuityCode::OrphanFuAFragment);
    const auto premature_marker = depacketizer.consume(
        packet(57, bytes({0x7C, 0x85, 0x01}), true));
    require(discontinuity(premature_marker[0],
                          "marker before FU-A end must be rejected")
                .code ==
            domain::H264DepacketizationDiscontinuityCode::MalformedFuA,
            "non-ending FU-A packet cannot carry the AU marker");

    const auto stats = depacketizer.stats();
    require(stats.dropped_packets == 8 && stats.unsupported_packets == 1 &&
                stats.orphan_fragments == 1,
            "invalid packet categories must update statistics");
}

void rejects_fragment_metadata_mismatch() {
    const auto check = [](const domain::RtpReorderEvent& continuation,
                          const domain::
                              H264DepacketizationDiscontinuityCode expected) {
        domain::H264RtpDepacketizer depacketizer;
        static_cast<void>(depacketizer.consume(
            packet(60, bytes({0x7C, 0x81, 0xAA}), false, 100)));
        const auto events = depacketizer.consume(continuation);
        require(events.size() == 1,
                "mismatched FU-A fragment must emit one discontinuity");
        require(discontinuity(events[0],
                              "mismatched fragment must be rejected")
                    .code == expected,
                "fragment mismatch reported the wrong reason");
        require(depacketizer.stats().pending_fragment_bytes == 0,
                "fragment mismatch must discard partial NAL state");
    };

    check(packet(62, bytes({0x7C, 0x41, 0xBB}), true, 100),
          domain::H264DepacketizationDiscontinuityCode::
              FragmentSequenceMismatch);
    check(packet(61, bytes({0x7C, 0x41, 0xBB}), true, 101),
          domain::H264DepacketizationDiscontinuityCode::
              FragmentTimestampMismatch);
    check(packet(61, bytes({0x5C, 0x41, 0xBB}), true, 100),
          domain::H264DepacketizationDiscontinuityCode::
              FragmentHeaderMismatch);
}

void bounds_reconstructed_nal_memory() {
    domain::H264RtpDepacketizer depacketizer{{4}};
    static_cast<void>(depacketizer.consume(
        packet(70, bytes({0x7C, 0x81, 0xAA, 0xBB}))));
    require(depacketizer.stats().pending_fragment_bytes == 3,
            "bounded FU-A start must retain reconstructed header and payload");

    const auto oversized = depacketizer.consume(
        packet(71, bytes({0x7C, 0x41, 0xCC, 0xDD}), true));
    require(discontinuity(oversized[0], "oversized FU-A must be rejected")
                .code == domain::H264DepacketizationDiscontinuityCode::
                             NalUnitTooLarge,
            "FU-A exceeding configured size must report a size issue");
    require(depacketizer.stats().pending_fragment_bytes == 0 &&
                depacketizer.stats().oversized_nal_units == 1,
            "oversized FU-A must release partial storage");

    require(!domain::validate_h264_rtp_depacketizer_config({0}),
            "zero maximum NAL size must be invalid");
    bool threw = false;
    try {
        domain::H264RtpDepacketizer invalid{{0}};
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "depacketizer constructor must reject zero size bound");
}

void reset_clears_partial_state_and_statistics() {
    domain::H264RtpDepacketizer depacketizer;
    static_cast<void>(depacketizer.consume(
        packet(80, bytes({0x7C, 0x81, 0xAA}))));
    depacketizer.reset();
    const auto stats = depacketizer.stats();
    require(stats.input_packets == 0 && stats.pending_fragment_bytes == 0,
            "reset must clear partial FU-A and per-session statistics");
}

}  // namespace

int main() {
    try {
        emits_single_nal_units_with_rtp_metadata();
        reconstructs_a_multi_packet_fu_a();
        reconstructs_fu_a_across_sequence_wraparound();
        sequence_gap_abandons_an_incomplete_fu_a();
        valid_packet_interrupts_an_unfinished_fu_a_then_continues();
        rejects_malformed_and_unsupported_packets();
        rejects_fragment_metadata_mismatch();
        bounds_reconstructed_nal_memory();
        reset_clears_partial_state_and_statistics();
    } catch (const std::exception& error) {
        std::cerr << "H.264 RTP depacketizer test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
