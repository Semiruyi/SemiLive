#pragma once

#include "publisher/model/media_clock.hpp"

#include <cstdint>

namespace semilive::publisher::domain {

[[nodiscard]] std::uint32_t media_clock_ticks_to_rtp_timestamp(
    model::MediaClockTicks ticks,
    std::uint32_t initial_timestamp) noexcept;

}  // namespace semilive::publisher::domain
