#include <semilive/publisher/model/media_clock.hpp>

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

using semilive::publisher::model::MediaClockRate;
using semilive::publisher::model::MediaClockTicks;
using semilive::publisher::model::MediaTime;
using semilive::publisher::model::MediaTimeConversionError;
using semilive::publisher::model::media_time_to_clock_ticks;
using namespace std::chrono_literals;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

void require_ticks(const MediaTime time,
                   const MediaClockRate rate,
                   const MediaClockTicks expected,
                   const std::string_view message) {
    const auto result = media_time_to_clock_ticks(time, rate);
    require(result.has_value() && *result == expected, message);
}

void conversion_rounds_to_the_nearest_tick() {
    require_ticks(1s, MediaClockRate{90'000}, MediaClockTicks{90'000},
                  "one second must map to 90000 video ticks");
    require_ticks(33'333'333ns, MediaClockRate{90'000},
                  MediaClockTicks{3'000},
                  "a 30 fps frame interval must round to 3000 video ticks");
    require_ticks(20ms, MediaClockRate{48'000}, MediaClockTicks{960},
                  "20 ms must map to 960 audio ticks at 48 kHz");
    require_ticks(5'555ns, MediaClockRate{90'000}, MediaClockTicks{0},
                  "a value below half a tick must round down");
    require_ticks(5'556ns, MediaClockRate{90'000}, MediaClockTicks{1},
                  "a value at or above half a tick must round up");
}

void invalid_inputs_return_typed_errors() {
    const auto invalid_rate =
        media_time_to_clock_ticks(1s, MediaClockRate{0});
    require(!invalid_rate &&
                invalid_rate.error() ==
                    MediaTimeConversionError::InvalidClockRate,
            "zero clock rate must return a typed error");

    const auto negative =
        media_time_to_clock_ticks(-1ns, MediaClockRate{90'000});
    require(!negative &&
                negative.error() == MediaTimeConversionError::NegativeTime,
            "negative media time must return a typed error");

    const auto overflow = media_time_to_clock_ticks(
        MediaTime::max(),
        MediaClockRate{std::numeric_limits<std::uint32_t>::max()});
    require(!overflow &&
                overflow.error() == MediaTimeConversionError::Overflow,
            "clock tick overflow must return a typed error");
}

}  // namespace

int main() {
    try {
        conversion_rounds_to_the_nearest_tick();
        invalid_inputs_return_typed_errors();
    } catch (const std::exception& error) {
        std::cerr << "publisher media clock tests failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher media clock tests passed\n";
    return EXIT_SUCCESS;
}
