#include "publisher/domain/timing/frame_scheduler.hpp"

#include <limits>
#include <stdexcept>
#include <type_traits>

namespace semilive::publisher::domain {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000;

using Clock = FrameScheduler::Clock;
using ClockRep = Clock::duration::rep;
using UnsignedClockRep = std::make_unsigned_t<ClockRep>;

static_assert(std::is_integral_v<ClockRep>);
static_assert(std::is_signed_v<ClockRep>);

UnsignedClockRep nonnegative_clock_difference(const Clock::time_point later,
                                              const Clock::time_point earlier) {
    const auto later_count = later.time_since_epoch().count();
    const auto earlier_count = earlier.time_since_epoch().count();

    if (later_count < earlier_count) {
        throw std::invalid_argument{"later clock time must not precede earlier clock time"};
    }
    if (earlier_count >= 0) {
        return static_cast<UnsignedClockRep>(later_count - earlier_count);
    }
    if (later_count < 0) {
        return static_cast<UnsignedClockRep>(later_count - earlier_count);
    }

    const auto earlier_magnitude =
        static_cast<UnsignedClockRep>(-(earlier_count + 1)) + 1;
    return static_cast<UnsignedClockRep>(later_count) + earlier_magnitude;
}

MediaTime media_time_between(const Clock::time_point later,
                             const Clock::time_point earlier) {
    const auto clock_ticks = nonnegative_clock_difference(later, earlier);
    if (clock_ticks > static_cast<UnsignedClockRep>(
                          std::numeric_limits<ClockRep>::max())) {
        throw std::overflow_error{"session media time is not representable"};
    }

    const Clock::duration clock_duration{static_cast<ClockRep>(clock_ticks)};
    const auto media_time = std::chrono::duration_cast<MediaTime>(clock_duration);
    if (media_time < MediaTime::zero() ||
        std::chrono::duration_cast<Clock::duration>(media_time) != clock_duration) {
        throw std::overflow_error{"session media time loses clock precision"};
    }
    return media_time;
}

Clock::duration clock_duration_for(const MediaTime media_time) {
    const auto clock_duration = std::chrono::duration_cast<Clock::duration>(media_time);
    if (clock_duration < Clock::duration::zero() ||
        std::chrono::duration_cast<MediaTime>(clock_duration) != media_time) {
        throw std::overflow_error{"frame offset is not representable by the steady clock"};
    }
    return clock_duration;
}

Clock::time_point add_clock_duration(const Clock::time_point start,
                                     const Clock::duration offset) {
    const auto start_count = start.time_since_epoch().count();
    const auto offset_count = offset.count();
    const auto maximum = std::numeric_limits<ClockRep>::max();

    if (offset_count < 0 ||
        (start_count >= 0 && offset_count > maximum - start_count)) {
        throw std::overflow_error{"frame deadline exceeds the steady clock range"};
    }
    return Clock::time_point{Clock::duration{start_count + offset_count}};
}

std::uint64_t rounded_divide(const std::uint64_t numerator,
                             const std::uint32_t denominator) {
    const auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    const auto round_up_threshold =
        static_cast<std::uint64_t>(denominator / 2U + denominator % 2U);
    return quotient + (remainder >= round_up_threshold ? 1U : 0U);
}

}  // namespace

FrameScheduler::FrameScheduler(const SessionTimeline timeline,
                               const FrameRate frame_rate,
                               const Clock::time_point track_start)
    : timeline_{timeline}, frame_rate_{frame_rate}, track_start_{track_start} {
    if (frame_rate_.numerator == 0 || frame_rate_.denominator == 0) {
        throw std::invalid_argument{"frame rate numerator and denominator must be non-zero"};
    }
    if (track_start_ < timeline_.origin()) {
        throw std::invalid_argument{"video track start must not precede session origin"};
    }

    nanoseconds_per_rate_numerator_ =
        static_cast<std::uint64_t>(frame_rate_.denominator) * kNanosecondsPerSecond;
    if (nanoseconds_per_rate_numerator_ < frame_rate_.numerator) {
        throw std::invalid_argument{
            "frame rate cannot produce strictly increasing nanosecond deadlines"};
    }

    first_presentation_time_ = media_time_between(track_start_, timeline_.origin());
    (void)make_tick(0);
    maximum_schedule_index_ = find_maximum_schedule_index();
}

FrameTick FrameScheduler::initial_tick() const {
    return make_tick(0);
}

FrameSchedule FrameScheduler::advance_after(const Clock::time_point now) {
    if (current_schedule_index_ == maximum_schedule_index_) {
        throw std::overflow_error{"frame schedule has reached its representable limit"};
    }

    const auto first_candidate = current_schedule_index_ + 1;
    auto selected_index = first_candidate;
    if (make_tick(first_candidate).deadline < now) {
        if (make_tick(maximum_schedule_index_).deadline < now) {
            throw std::overflow_error{"no representable frame deadline remains"};
        }

        auto lower = first_candidate;
        auto upper = maximum_schedule_index_;
        while (lower < upper) {
            const auto middle = lower + (upper - lower) / 2;
            if (make_tick(middle).deadline < now) {
                lower = middle + 1;
            } else {
                upper = middle;
            }
        }
        selected_index = lower;
    }

    const auto skipped_ticks = selected_index - current_schedule_index_ - 1;
    current_schedule_index_ = selected_index;
    return FrameSchedule{make_tick(selected_index), skipped_ticks};
}

MediaTime FrameScheduler::frame_offset(const std::uint64_t schedule_index) const {
    const auto rate_numerator = static_cast<std::uint64_t>(frame_rate_.numerator);
    const auto whole_rate_periods = schedule_index / rate_numerator;
    const auto remaining_index = schedule_index % rate_numerator;
    const auto maximum_nanoseconds =
        static_cast<std::uint64_t>(std::numeric_limits<MediaTime::rep>::max());

    if (whole_rate_periods > maximum_nanoseconds / nanoseconds_per_rate_numerator_) {
        throw std::overflow_error{"frame offset exceeds MediaTime range"};
    }
    const auto whole_nanoseconds =
        whole_rate_periods * nanoseconds_per_rate_numerator_;

    const auto scale_quotient = nanoseconds_per_rate_numerator_ / rate_numerator;
    const auto scale_remainder = nanoseconds_per_rate_numerator_ % rate_numerator;
    const auto fractional_whole = remaining_index * scale_quotient;
    const auto fractional_numerator = remaining_index * scale_remainder;
    const auto fractional_nanoseconds =
        fractional_whole + rounded_divide(fractional_numerator, frame_rate_.numerator);

    if (fractional_nanoseconds > maximum_nanoseconds - whole_nanoseconds) {
        throw std::overflow_error{"frame offset exceeds MediaTime range"};
    }
    return MediaTime{static_cast<MediaTime::rep>(whole_nanoseconds + fractional_nanoseconds)};
}

FrameTick FrameScheduler::make_tick(const std::uint64_t schedule_index) const {
    const auto offset = frame_offset(schedule_index);
    const auto maximum_media_time = std::numeric_limits<MediaTime::rep>::max();
    if (offset.count() > maximum_media_time - first_presentation_time_.count()) {
        throw std::overflow_error{"frame presentation time exceeds MediaTime range"};
    }

    return FrameTick{
        schedule_index,
        add_clock_duration(track_start_, clock_duration_for(offset)),
        first_presentation_time_ + offset,
    };
}

bool FrameScheduler::can_represent(const std::uint64_t schedule_index) const noexcept {
    try {
        (void)make_tick(schedule_index);
        return true;
    } catch (...) {
        return false;
    }
}

std::uint64_t FrameScheduler::find_maximum_schedule_index() const noexcept {
    std::uint64_t lower = 0;
    auto upper = std::numeric_limits<std::uint64_t>::max();

    while (lower < upper) {
        const auto distance = upper - lower;
        const auto middle = lower + distance / 2 + distance % 2;
        if (can_represent(middle)) {
            lower = middle;
        } else {
            upper = middle - 1;
        }
    }
    return lower;
}

}  // namespace semilive::publisher::domain
