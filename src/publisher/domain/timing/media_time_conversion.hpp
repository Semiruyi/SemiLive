#pragma once

#include "publisher/model/media_time.hpp"

#include <cstdint>

namespace semilive::publisher::domain {

[[nodiscard]] std::uint64_t media_time_to_clock_ticks(model::MediaTime media_time,
                                                      std::uint32_t clock_rate);

[[nodiscard]] std::uint32_t media_time_to_rtp_timestamp(model::MediaTime media_time,
                                                        std::uint32_t clock_rate,
                                                        std::uint32_t initial_timestamp);

}  // namespace semilive::publisher::domain
