#include "publisher/domain/rtp/rtp_timestamp.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using semilive::publisher::domain::media_clock_ticks_to_rtp_timestamp;
using semilive::publisher::model::MediaClockTicks;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

void rtp_timestamp_wraps_modulo_32_bits() {
    constexpr std::uint32_t kInitialTimestamp =
        std::numeric_limits<std::uint32_t>::max() - 44U;
    require(media_clock_ticks_to_rtp_timestamp(
                MediaClockTicks{90'000}, kInitialTimestamp) == 89'955,
            "RTP timestamp addition must wrap modulo 32 bits");
}

}  // namespace

int main() {
    try {
        rtp_timestamp_wraps_modulo_32_bits();
    } catch (const std::exception& error) {
        std::cerr << "publisher RTP timestamp tests failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher RTP timestamp tests passed\n";
    return EXIT_SUCCESS;
}
