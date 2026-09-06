#pragma once

#include "publisher/model/media_time.hpp"

#include <compare>
#include <cstdint>
#include <expected>

namespace semilive::publisher::model {

struct MediaClockRate {
    std::uint32_t ticks_per_second = 0;

    [[nodiscard]] friend constexpr bool operator==(
        const MediaClockRate&,
        const MediaClockRate&) = default;
};

struct MediaClockTicks {
    std::uint64_t value = 0;

    [[nodiscard]] friend constexpr auto operator<=> (
        const MediaClockTicks&,
        const MediaClockTicks&) = default;
};

enum class MediaTimeConversionError : std::uint8_t {
    InvalidClockRate,
    NegativeTime,
    Overflow,
};

[[nodiscard]] std::expected<MediaClockTicks, MediaTimeConversionError>
media_time_to_clock_ticks(MediaTime media_time,
                          MediaClockRate clock_rate) noexcept;

}  // namespace semilive::publisher::model
