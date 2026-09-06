#include "publisher/model/media_clock.hpp"

#include <cstdint>
#include <limits>

namespace semilive::publisher::model {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000;

}  // namespace

std::expected<MediaClockTicks, MediaTimeConversionError>
media_time_to_clock_ticks(const MediaTime media_time,
                          const MediaClockRate clock_rate) noexcept {
    if (clock_rate.ticks_per_second == 0) {
        return std::unexpected{MediaTimeConversionError::InvalidClockRate};
    }
    if (media_time < MediaTime::zero()) {
        return std::unexpected{MediaTimeConversionError::NegativeTime};
    }

    const auto nanoseconds = static_cast<std::uint64_t>(media_time.count());
    const auto whole_seconds = nanoseconds / kNanosecondsPerSecond;
    const auto remaining_nanoseconds = nanoseconds % kNanosecondsPerSecond;
    const auto rate =
        static_cast<std::uint64_t>(clock_rate.ticks_per_second);
    const auto rounded_fraction =
        (remaining_nanoseconds * rate + kNanosecondsPerSecond / 2U) /
        kNanosecondsPerSecond;

    constexpr auto kMaximum = std::numeric_limits<std::uint64_t>::max();
    if (whole_seconds > (kMaximum - rounded_fraction) / rate) {
        return std::unexpected{MediaTimeConversionError::Overflow};
    }
    return MediaClockTicks{whole_seconds * rate + rounded_fraction};
}

}  // namespace semilive::publisher::model
