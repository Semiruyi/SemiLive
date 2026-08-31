#pragma once

#include <chrono>
#include <cstdint>

namespace semilive::publisher::domain {

using MediaTime = std::chrono::nanoseconds;

[[nodiscard]] std::uint64_t media_time_to_clock_ticks(MediaTime media_time,
                                                      std::uint32_t clock_rate);

[[nodiscard]] std::uint32_t media_time_to_rtp_timestamp(MediaTime media_time,
                                                        std::uint32_t clock_rate,
                                                        std::uint32_t initial_timestamp);

}  // namespace semilive::publisher::domain
