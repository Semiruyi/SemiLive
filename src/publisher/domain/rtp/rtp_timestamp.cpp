#include "publisher/domain/rtp/rtp_timestamp.hpp"

#include <cstdint>

namespace semilive::publisher::domain {

std::uint32_t media_clock_ticks_to_rtp_timestamp(
    const model::MediaClockTicks ticks,
    const std::uint32_t initial_timestamp) noexcept {
    return initial_timestamp + static_cast<std::uint32_t>(ticks.value);
}

}  // namespace semilive::publisher::domain
