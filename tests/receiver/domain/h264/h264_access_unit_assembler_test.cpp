#include <semilive/receiver/domain/h264/h264_access_unit_assembler.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <optional>
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

using Clock = model::H264NalUnit::Clock;

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

[[nodiscard]] domain::H264DepacketizerEvent nal_event(
    const std::uint16_t first_sequence,
    const std::uint16_t last_sequence,
    const std::uint32_t timestamp,
    const bool marker,
    std::vector<std::byte> nal_bytes,
    const std::chrono::milliseconds completed_at = 0ms) {
    return model::H264NalUnit{
        std::move(nal_bytes), timestamp, marker, first_sequence,
        last_sequence, Clock::time_point{completed_at}};
}

[[nodiscard]] const model::H264AccessUnit& access_unit(
    const domain::H264AccessUnitAssemblerEvent& event,
    const std::string_view message) {
    const auto* value = std::get_if<model::H264AccessUnit>(&event);
    require(value != nullptr, message);
    return *value;
}

[[nodiscard]] const domain::H264AccessUnitDiscontinuity& discontinuity(
    const domain::H264AccessUnitAssemblerEvent& event,
    const std::string_view message) {
    const auto* value =
        std::get_if<domain::H264AccessUnitDiscontinuity>(&event);
    require(value != nullptr, message);
    return *value;
}

void emits_a_marker_terminated_single_nal_access_unit() {
    domain::H264AccessUnitAssembler assembler;
    const auto events = assembler.consume(
        nal_event(10, 10, 1000, true, bytes({0x61, 0xAA}), 7ms));

    require(events.size() == 1, "marker must complete a one-NAL AU");
    const auto& au = access_unit(events[0], "marker output must be an AU");
    require(std::ranges::equal(
                au.annex_b(), bytes({0x00, 0x00, 0x00, 0x01, 0x61, 0xAA})),
            "assembler must prepend a four-byte Annex-B start code");
    require(au.rtp_timestamp() == 1000 && au.first_sequence() == 10 &&
                au.last_sequence() == 10 && au.nal_unit_count() == 1,
            "one-NAL AU metadata must be preserved");
    require(au.contains_vcl() && !au.contains_idr() &&
                !au.is_random_access_candidate(),
            "non-IDR slice flags must be classified");
    require(au.completed_at() == Clock::time_point{7ms},
            "AU completion time must come from the marker NAL");
}

void assembles_sps_pps_and_idr_in_order() {
    domain::H264AccessUnitAssembler assembler;
    require(assembler.consume(
                nal_event(20, 20, 2000, false, bytes({0x67, 0x11})))
                .empty(),
            "SPS must wait for the AU marker");
    require(assembler.consume(
                nal_event(21, 21, 2000, false, bytes({0x68, 0x22})))
                .empty(),
            "PPS must wait for the AU marker");

    const auto events = assembler.consume(
        nal_event(22, 24, 2000, true, bytes({0x65, 0x33, 0x44}), 9ms));
    require(events.size() == 1, "IDR marker must complete the AU");
    const auto& au = access_unit(events[0], "IDR output must be an AU");
    require(std::ranges::equal(
                au.annex_b(),
                bytes({0x00, 0x00, 0x00, 0x01, 0x67, 0x11,
                       0x00, 0x00, 0x00, 0x01, 0x68, 0x22,
                       0x00, 0x00, 0x00, 0x01, 0x65, 0x33, 0x44})),
            "AU must preserve NAL order and Annex-B boundaries");
    require(au.nal_unit_count() == 3 && au.first_sequence() == 20 &&
                au.last_sequence() == 24,
            "multi-NAL AU must retain sequence range and NAL count");
    require(au.contains_sps() && au.contains_pps() && au.contains_idr() &&
                au.contains_vcl() && au.is_random_access_candidate(),
            "SPS/PPS/IDR AU must be classified as random access candidate");
}

void timestamp_change_discards_unterminated_au_but_accepts_new_au() {
    domain::H264AccessUnitAssembler assembler;
    static_cast<void>(assembler.consume(
        nal_event(30, 30, 3000, false, bytes({0x61, 0xAA}))));

    const auto events = assembler.consume(
        nal_event(31, 31, 4000, true, bytes({0x61, 0xBB})));
    require(events.size() == 2,
            "timestamp change must report damage then emit the new AU");
    const auto& issue = discontinuity(
        events[0], "timestamp change must emit a discontinuity first");
    require(issue.code == domain::H264AccessUnitDiscontinuityCode::
                              TimestampChangedBeforeMarker &&
                issue.related_timestamp == 3000,
            "timestamp discontinuity must identify the abandoned AU");
    require(access_unit(events[1], "new timestamp AU must still complete")
                .rtp_timestamp() == 4000,
            "new timestamp must start a fresh AU");
}

void upstream_discontinuity_discards_the_known_affected_timestamp() {
    domain::H264AccessUnitAssembler assembler;
    static_cast<void>(assembler.consume(
        nal_event(40, 40, 5000, false, bytes({0x67, 0x11}))));

    const auto issue = assembler.consume(
        domain::H264DepacketizationDiscontinuity{
            domain::H264DepacketizationDiscontinuityCode::SequenceGap,
            41,
            1});
    require(issue.size() == 1,
            "upstream discontinuity must be propagated immediately");
    const auto& propagated = discontinuity(
        issue[0], "upstream issue must become an AU discontinuity");
    require(propagated.code ==
                domain::H264AccessUnitDiscontinuityCode::
                    UpstreamDiscontinuity &&
                propagated.upstream_code ==
                    domain::H264DepacketizationDiscontinuityCode::SequenceGap,
            "assembler must preserve the upstream discontinuity reason");

    require(assembler.consume(
                nal_event(42, 42, 5000, true, bytes({0x65, 0x22})))
                .empty(),
            "remaining NALs from the damaged timestamp must be discarded");
    const auto next = assembler.consume(
        nal_event(43, 43, 6000, true, bytes({0x61, 0x33})));
    require(next.size() == 1 &&
                access_unit(next[0], "next timestamp must recover assembly")
                        .rtp_timestamp() == 6000,
            "assembler must resume at the next complete timestamp");
}

void discontinuity_without_pending_state_discards_one_unknown_au() {
    domain::H264AccessUnitAssembler assembler;
    static_cast<void>(assembler.consume(
        domain::H264DepacketizationDiscontinuity{
            domain::H264DepacketizationDiscontinuityCode::SequenceGap,
            50,
            2}));

    require(assembler.consume(
                nal_event(52, 52, 7000, false, bytes({0x67, 0x11})))
                .empty(),
            "first post-gap NAL must establish the discarded timestamp");
    require(assembler.consume(
                nal_event(53, 53, 7000, true, bytes({0x65, 0x22})))
                .empty(),
            "entire first post-gap AU must be discarded conservatively");
    const auto next = assembler.consume(
        nal_event(54, 54, 8000, true, bytes({0x61, 0x33})));
    require(next.size() == 1,
            "assembler must resume after the discarded AU marker");
    require(assembler.stats().discarded_access_units == 1 &&
                assembler.stats().discarded_nal_units == 2,
            "conservatively discarded unknown AU must be counted once");
}

void sequence_mismatch_discards_the_rest_of_the_au() {
    domain::H264AccessUnitAssembler assembler;
    static_cast<void>(assembler.consume(
        nal_event(60, 60, 9000, false, bytes({0x67, 0x11}))));

    const auto mismatch = assembler.consume(
        nal_event(62, 62, 9000, false, bytes({0x68, 0x22})));
    require(mismatch.size() == 1 &&
                discontinuity(mismatch[0], "sequence mismatch must fail AU")
                        .code == domain::
                                     H264AccessUnitDiscontinuityCode::
                                         NalSequenceMismatch,
            "NAL sequence hole must be reported explicitly");
    require(assembler.consume(
                nal_event(63, 63, 9000, true, bytes({0x65, 0x33})))
                .empty(),
            "marker from sequence-damaged AU must not emit an AU");
}

void bounds_access_unit_size_and_nal_count() {
    domain::H264AccessUnitAssembler size_bounded{{11, 8}};
    static_cast<void>(size_bounded.consume(
        nal_event(70, 70, 10'000, false, bytes({0x67, 0x11}))));
    const auto too_large = size_bounded.consume(
        nal_event(71, 71, 10'000, false, bytes({0x68, 0x22})));
    require(discontinuity(too_large[0], "oversized AU must fail")
                .code ==
            domain::H264AccessUnitDiscontinuityCode::AccessUnitTooLarge,
            "AU byte limit must be enforced before append");
    require(size_bounded.consume(
                nal_event(72, 72, 10'000, true, bytes({0x65})))
                .empty(),
            "remainder of oversized AU must be discarded");

    domain::H264AccessUnitAssembler count_bounded{{1024, 1}};
    static_cast<void>(count_bounded.consume(
        nal_event(80, 80, 11'000, false, bytes({0x67}))));
    const auto too_many = count_bounded.consume(
        nal_event(81, 81, 11'000, true, bytes({0x65})));
    require(discontinuity(too_many[0], "excess NAL count must fail")
                .code ==
            domain::H264AccessUnitDiscontinuityCode::TooManyNalUnits,
            "AU NAL count limit must be enforced before append");

    require(!domain::validate_h264_access_unit_assembler_config({4, 1}),
            "AU must fit at least a start code and one NAL byte");
    require(!domain::validate_h264_access_unit_assembler_config({1024, 0}),
            "zero NAL count limit must be invalid");
}

void reset_clears_pending_and_discarding_state() {
    domain::H264AccessUnitAssembler assembler;
    static_cast<void>(assembler.consume(
        nal_event(90, 90, 12'000, false, bytes({0x67}))));
    assembler.reset();

    const auto stats = assembler.stats();
    require(stats.input_nal_units == 0 && stats.pending_bytes == 0 &&
                stats.pending_nal_units == 0,
            "reset must clear pending AU and per-session statistics");
    const auto fresh = assembler.consume(
        nal_event(500, 500, 13'000, true, bytes({0x61})));
    require(fresh.size() == 1,
            "reset must permit an unrelated fresh access unit");
}

}  // namespace

int main() {
    try {
        emits_a_marker_terminated_single_nal_access_unit();
        assembles_sps_pps_and_idr_in_order();
        timestamp_change_discards_unterminated_au_but_accepts_new_au();
        upstream_discontinuity_discards_the_known_affected_timestamp();
        discontinuity_without_pending_state_discards_one_unknown_au();
        sequence_mismatch_discards_the_rest_of_the_au();
        bounds_access_unit_size_and_nal_count();
        reset_clears_pending_and_discarding_state();
    } catch (const std::exception& error) {
        std::cerr << "H.264 access unit assembler test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
