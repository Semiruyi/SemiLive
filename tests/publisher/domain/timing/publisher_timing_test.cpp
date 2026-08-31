#include "publisher/domain/timing/media_time.hpp"
#include "publisher/domain/timing/session_timeline.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using semilive::publisher::domain::MediaTime;
using semilive::publisher::domain::SessionTimeline;
using semilive::publisher::domain::media_time_to_clock_ticks;
using semilive::publisher::domain::media_time_to_rtp_timestamp;
using namespace std::chrono_literals;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename Exception, typename Operation>
void require_throws(Operation&& operation, const std::string_view message) {
    try {
        operation();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error{std::string{message}};
}

void session_timeline_uses_immutable_origin() {
    const SessionTimeline::Clock::time_point origin{10s};
    const SessionTimeline timeline{origin};

    require(timeline.origin() == origin, "timeline must retain its construction origin");
    require(timeline.media_time_at(origin) == MediaTime::zero(),
            "session origin must map to zero media time");
    require(timeline.media_time_at(origin + 25ms) == 25ms,
            "time after the origin must map to a positive media offset");
    require(timeline.media_time_at(origin - 1ms) == -1ms,
            "timeline must preserve negative calibration offsets");
}

void clock_tick_conversion_rounds_to_nearest_tick() {
    require(media_time_to_clock_ticks(1s, 90'000) == 90'000,
            "one second must map to 90000 video ticks");
    require(media_time_to_clock_ticks(33'333'333ns, 90'000) == 3'000,
            "a 30 fps frame interval must round to 3000 video ticks");
    require(media_time_to_clock_ticks(20ms, 48'000) == 960,
            "20 ms must map to 960 audio ticks at 48 kHz");
    require(media_time_to_clock_ticks(5'555ns, 90'000) == 0,
            "a value below half a tick must round down");
    require(media_time_to_clock_ticks(5'556ns, 90'000) == 1,
            "a value at or above half a tick must round up");
}

void invalid_clock_tick_inputs_are_rejected() {
    require_throws<std::invalid_argument>(
        [] { (void)media_time_to_clock_ticks(1s, 0); },
        "zero clock rate must be rejected");
    require_throws<std::invalid_argument>(
        [] { (void)media_time_to_clock_ticks(-1ns, 90'000); },
        "negative published media time must be rejected");
    require_throws<std::overflow_error>(
        [] {
            (void)media_time_to_clock_ticks(
                MediaTime::max(), std::numeric_limits<std::uint32_t>::max());
        },
        "clock tick overflow must be rejected");
}

void rtp_timestamp_wraps_modulo_32_bits() {
    constexpr std::uint32_t kInitialTimestamp =
        std::numeric_limits<std::uint32_t>::max() - 44;
    require(media_time_to_rtp_timestamp(1s, 90'000, kInitialTimestamp) == 89'955,
            "RTP timestamp addition must wrap modulo 32 bits");
}

}  // namespace

int main() {
    try {
        session_timeline_uses_immutable_origin();
        clock_tick_conversion_rounds_to_nearest_tick();
        invalid_clock_tick_inputs_are_rejected();
        rtp_timestamp_wraps_modulo_32_bits();
    } catch (const std::exception& error) {
        std::cerr << "publisher timing test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher timing tests passed\n";
    return EXIT_SUCCESS;
}
