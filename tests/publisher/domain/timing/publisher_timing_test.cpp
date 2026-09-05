#include "publisher/domain/timing/frame_scheduler.hpp"
#include "publisher/domain/timing/media_time_conversion.hpp"
#include "publisher/domain/timing/session_timeline.hpp"
#include "publisher/model/media_time.hpp"
#include "publisher/model/video/frame_rate.hpp"

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

using semilive::publisher::domain::FrameScheduler;
using semilive::publisher::domain::SessionTimeline;
using semilive::publisher::domain::media_time_to_clock_ticks;
using semilive::publisher::domain::media_time_to_rtp_timestamp;
using semilive::publisher::model::FrameRate;
using semilive::publisher::model::MediaTime;
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

void frame_scheduler_generates_rounded_absolute_deadlines() {
    const FrameScheduler::Clock::time_point origin{10s};
    FrameScheduler scheduler{SessionTimeline{origin}, FrameRate{30, 1}, origin};

    const auto initial = scheduler.initial_tick();
    require(initial.schedule_index == 0, "initial schedule index must be zero");
    require(initial.deadline == origin, "initial deadline must equal track start");
    require(initial.presentation_time == 0ns, "initial PTS must equal track start offset");

    const auto first = scheduler.advance_after(initial.deadline);
    const auto second = scheduler.advance_after(first.tick.deadline);
    const auto third = scheduler.advance_after(second.tick.deadline);
    require(first.tick.deadline == origin + 33'333'333ns,
            "30 fps tick 1 must round down to the nearest nanosecond");
    require(second.tick.deadline == origin + 66'666'667ns,
            "30 fps tick 2 must round up to the nearest nanosecond");
    require(third.tick.deadline == origin + 100ms,
            "30 fps tick 3 must land exactly at 100 ms");
    require(first.skipped_ticks == 0 && second.skipped_ticks == 0 &&
                third.skipped_ticks == 0,
            "normal advancement must not skip scheduling ticks");

    FrameScheduler one_second_scheduler{
        SessionTimeline{origin}, FrameRate{30, 1}, origin};
    const auto one_second = one_second_scheduler.advance_after(origin + 1s);
    require(one_second.tick.schedule_index == 30 &&
                one_second.tick.deadline == origin + 1s,
            "30 fps tick 30 must land exactly at one second without drift");
}

void frame_scheduler_preserves_track_start_media_offset() {
    const FrameScheduler::Clock::time_point origin{10s};
    const auto track_start = origin + 250ms;
    FrameScheduler scheduler{SessionTimeline{origin}, FrameRate{30, 1}, track_start};

    const auto initial = scheduler.initial_tick();
    require(initial.deadline == track_start, "first deadline must equal video track start");
    require(initial.presentation_time == 250ms,
            "first PTS must retain the track start offset from session origin");
}

void frame_scheduler_skips_expired_ticks_without_burst_catch_up() {
    const FrameScheduler::Clock::time_point origin{10s};
    FrameScheduler scheduler{SessionTimeline{origin}, FrameRate{30, 1}, origin};

    const auto first = scheduler.advance_after(origin);
    const auto second = scheduler.advance_after(first.tick.deadline);
    const auto third = scheduler.advance_after(second.tick.deadline);
    require(third.tick.schedule_index == 3,
            "test setup must process tick 3 before simulating late work");

    const auto schedule = scheduler.advance_after(origin + 180ms);
    require(schedule.tick.schedule_index == 6,
            "first non-expired tick at 180 ms must be tick 6");
    require(schedule.tick.deadline == origin + 200ms,
            "late advancement must return the first future deadline");
    require(schedule.tick.presentation_time == 200ms,
            "skipped ticks must leave a real PTS gap");
    require(schedule.skipped_ticks == 2,
            "advancing from tick 3 to tick 6 must skip ticks 4 and 5");
}

void frame_scheduler_keeps_deadline_equal_to_now() {
    const FrameScheduler::Clock::time_point origin{10s};
    FrameScheduler scheduler{SessionTimeline{origin}, FrameRate{30, 1}, origin};

    const auto schedule = scheduler.advance_after(origin + 100ms);
    require(schedule.tick.schedule_index == 3,
            "a deadline equal to now must remain eligible");
    require(schedule.skipped_ticks == 2,
            "only deadlines strictly earlier than now must be skipped");
}

void frame_scheduler_avoids_fractional_rate_drift() {
    const FrameScheduler::Clock::time_point origin{10s};
    FrameScheduler scheduler{
        SessionTimeline{origin}, FrameRate{30'000, 1'001}, origin};

    const auto schedule = scheduler.advance_after(origin + 1'001s);
    require(schedule.tick.schedule_index == 30'000,
            "30000/1001 fps must reach tick 30000 after 1001 seconds");
    require(schedule.tick.deadline == origin + 1'001s,
            "fractional frame rate deadlines must not accumulate rounding drift");
    require(schedule.tick.presentation_time == 1'001s,
            "fractional frame rate PTS must use the absolute index formula");
    require(schedule.skipped_ticks == 29'999,
            "direct late advancement must report every omitted scheduling tick");
}

void invalid_frame_scheduler_inputs_are_rejected() {
    const FrameScheduler::Clock::time_point origin{10s};
    require_throws<std::invalid_argument>(
        [origin] {
            [[maybe_unused]] FrameScheduler scheduler{
                SessionTimeline{origin}, FrameRate{0, 1}, origin};
        },
        "zero frame rate numerator must be rejected");
    require_throws<std::invalid_argument>(
        [origin] {
            [[maybe_unused]] FrameScheduler scheduler{
                SessionTimeline{origin}, FrameRate{30, 0}, origin};
        },
        "zero frame rate denominator must be rejected");
    require_throws<std::invalid_argument>(
        [origin] {
            [[maybe_unused]] FrameScheduler scheduler{
                SessionTimeline{origin}, FrameRate{1'000'000'001, 1}, origin};
        },
        "frame rates above nanosecond resolution must be rejected");
    require_throws<std::invalid_argument>(
        [origin] {
            [[maybe_unused]] FrameScheduler scheduler{
                SessionTimeline{origin}, FrameRate{30, 1}, origin - 1ns};
        },
        "track start before session origin must be rejected");
}

void exhausted_frame_schedule_is_reported() {
    const auto final_time = FrameScheduler::Clock::time_point::max();
    FrameScheduler scheduler{
        SessionTimeline{final_time}, FrameRate{30, 1}, final_time};

    require_throws<std::overflow_error>(
        [&scheduler] { (void)scheduler.advance_after(FrameScheduler::Clock::time_point::max()); },
        "advancing beyond the representable clock range must fail");
}

}  // namespace

int main() {
    try {
        session_timeline_uses_immutable_origin();
        clock_tick_conversion_rounds_to_nearest_tick();
        invalid_clock_tick_inputs_are_rejected();
        rtp_timestamp_wraps_modulo_32_bits();
        frame_scheduler_generates_rounded_absolute_deadlines();
        frame_scheduler_preserves_track_start_media_offset();
        frame_scheduler_skips_expired_ticks_without_burst_catch_up();
        frame_scheduler_keeps_deadline_equal_to_now();
        frame_scheduler_avoids_fractional_rate_drift();
        invalid_frame_scheduler_inputs_are_rejected();
        exhausted_frame_schedule_is_reported();
    } catch (const std::exception& error) {
        std::cerr << "publisher timing test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher timing tests passed\n";
    return EXIT_SUCCESS;
}
