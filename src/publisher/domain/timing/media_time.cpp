#include "publisher/domain/timing/media_time.hpp"

#include <limits>
#include <stdexcept>

namespace semilive::publisher::domain {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000;

}  // namespace

std::uint64_t media_time_to_clock_ticks(const MediaTime media_time,
                                        const std::uint32_t clock_rate) {
    if (clock_rate == 0) {
        throw std::invalid_argument{"media clock rate must be greater than zero"};
    }
    if (media_time < MediaTime::zero()) {
        throw std::invalid_argument{"published media time must not be negative"};
    }

    const auto nanoseconds = static_cast<std::uint64_t>(media_time.count());
    const auto whole_seconds = nanoseconds / kNanosecondsPerSecond;
    const auto remaining_nanoseconds = nanoseconds % kNanosecondsPerSecond;
    const auto rate = static_cast<std::uint64_t>(clock_rate);
    const auto rounded_fraction =
        (remaining_nanoseconds * rate + kNanosecondsPerSecond / 2) /
        kNanosecondsPerSecond;

    constexpr auto kMaximum = std::numeric_limits<std::uint64_t>::max();
    if (whole_seconds > (kMaximum - rounded_fraction) / rate) {
        throw std::overflow_error{"media clock tick conversion overflow"};
    }

    return whole_seconds * rate + rounded_fraction;
}

std::uint32_t media_time_to_rtp_timestamp(const MediaTime media_time,
                                          const std::uint32_t clock_rate,
                                          const std::uint32_t initial_timestamp) {
    const auto ticks = media_time_to_clock_ticks(media_time, clock_rate);
    return initial_timestamp + static_cast<std::uint32_t>(ticks);
}

}  // namespace semilive::publisher::domain
