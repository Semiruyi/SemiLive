#include <semilive/receiver/domain/rtp/rtp_timestamp_mapper.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace domain = semilive::receiver::domain;
namespace model = semilive::receiver::model;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

[[nodiscard]] model::PlayableH264AccessUnit access_unit(
    const std::uint32_t timestamp,
    const bool discontinuity_before = false) {
    return {
        model::H264AccessUnit{
            std::vector<std::byte>{std::byte{0x00}, std::byte{0x00},
                                   std::byte{0x00}, std::byte{0x01},
                                   std::byte{0x65}},
            timestamp,
            10,
            10,
            model::H264AccessUnit::Clock::time_point{},
            1,
            true,
            true,
            true,
            true},
        discontinuity_before};
}

void maps_the_first_timestamp_to_zero_and_converts_90_khz_ticks() {
    domain::RtpTimestampMapper mapper;

    auto first = mapper.map(access_unit(123'456, true));
    require(first.has_value(), "first RTP timestamp must establish an origin");
    require(first->presentation_time() == 0ns &&
                first->extended_rtp_timestamp() == 123'456,
            "first AU must map to time zero without changing its RTP epoch");
    require(first->discontinuity_before(),
            "timestamp mapping must preserve the recovery marker");

    auto second = mapper.map(access_unit(126'456));
    require(second.has_value(), "increasing RTP timestamp must map");
    require(second->presentation_time() == 33'333'333ns,
            "3000 ticks at 90 kHz must map to one thirtieth second");
    require(second->access_unit().rtp_timestamp() == 126'456,
            "mapper must preserve the owned access unit");
}

void extends_natural_32_bit_timestamp_wraparound() {
    domain::RtpTimestampMapper mapper;
    constexpr std::uint32_t first_timestamp = 0xffff'ff00U;

    auto first = mapper.map(access_unit(first_timestamp));
    auto wrapped = mapper.map(access_unit(0x0000'0284U));
    require(first && wrapped, "natural RTP wraparound must map successfully");
    require(wrapped->extended_rtp_timestamp() ==
                static_cast<std::uint64_t>(first_timestamp) + 900U,
            "extended timestamp must remain increasing across wraparound");
    require(wrapped->presentation_time() == 10ms,
            "wrapped delta must retain its media duration");
    require(mapper.stats().timestamp_wraps == 1,
            "timestamp wraparound must be observable");
}

void preserves_sender_timeline_across_decoder_discontinuity() {
    domain::RtpTimestampMapper mapper;
    static_cast<void>(mapper.map(access_unit(10'000)));
    auto recovered = mapper.map(access_unit(190'000, true));

    require(recovered && recovered->presentation_time() == 2s,
            "recovery must retain elapsed sender time for future A/V sync");
    require(recovered->discontinuity_before(),
            "recovered AU must still tell the player to re-anchor");
}

void rejects_non_increasing_timestamps_without_poisoning_state() {
    domain::RtpTimestampMapper mapper;
    static_cast<void>(mapper.map(access_unit(50'000)));

    const auto duplicate = mapper.map(access_unit(50'000));
    require(!duplicate &&
                duplicate.error().code ==
                    domain::RtpTimestampMappingErrorCode::
                        NonIncreasingTimestamp,
            "duplicate AU timestamp must be rejected");
    const auto backward = mapper.map(access_unit(49'000));
    require(!backward &&
                backward.error().code ==
                    domain::RtpTimestampMappingErrorCode::
                        NonIncreasingTimestamp,
            "small backward timestamp jump must be rejected");

    auto next = mapper.map(access_unit(59'000));
    require(next && next->presentation_time() == 100ms,
            "rejected timestamps must not alter the valid mapping origin");
    const auto stats = mapper.stats();
    require(stats.input_access_units == 4 && stats.mapped_access_units == 2 &&
                stats.non_increasing_timestamps == 2,
            "mapping failures must be counted separately from outputs");
}

void validates_clock_rate_and_reports_media_time_overflow() {
    require(!domain::validate_rtp_timestamp_mapper_config({0}),
            "zero RTP clock rate must be invalid");

    domain::RtpTimestampMapper mapper{{1}};
    std::uint32_t timestamp = 0;
    require(mapper.map(access_unit(timestamp)).has_value(),
            "one-hertz mapper must establish its origin");
    for (int index = 0; index < 4; ++index) {
        timestamp += 0x7fff'ffffU;
        require(mapper.map(access_unit(timestamp)).has_value(),
                "representable long media time must map");
    }
    timestamp += 0x7fff'ffffU;
    const auto overflow = mapper.map(access_unit(timestamp));
    require(!overflow &&
                overflow.error().code ==
                    domain::RtpTimestampMappingErrorCode::MediaTimeOverflow,
            "media time beyond nanosecond representation must fail safely");
}

void reset_starts_a_fresh_timestamp_epoch() {
    domain::RtpTimestampMapper mapper;
    static_cast<void>(mapper.map(access_unit(90'000)));
    static_cast<void>(mapper.map(access_unit(180'000)));
    mapper.reset();

    const auto stats = mapper.stats();
    require(stats.input_access_units == 0 &&
                stats.mapped_access_units == 0 &&
                !stats.last_extended_timestamp,
            "reset must clear per-session timestamp statistics");
    auto fresh = mapper.map(access_unit(42));
    require(fresh && fresh->presentation_time() == 0ns,
            "new session must accept an unrelated RTP epoch at time zero");
}

}  // namespace

int main() {
    try {
        maps_the_first_timestamp_to_zero_and_converts_90_khz_ticks();
        extends_natural_32_bit_timestamp_wraparound();
        preserves_sender_timeline_across_decoder_discontinuity();
        rejects_non_increasing_timestamps_without_poisoning_state();
        validates_clock_rate_and_reports_media_time_overflow();
        reset_starts_a_fresh_timestamp_epoch();
    } catch (const std::exception& error) {
        std::cerr << "RTP timestamp mapper test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
