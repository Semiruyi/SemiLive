#include <semilive/common/rtcp/ntp_time.hpp>

#include <chrono>
#include <cstdint>
#include <limits>

namespace semilive::common::rtcp {
namespace {

constexpr std::int64_t ntp_unix_epoch_offset_seconds = 2'208'988'800LL;
constexpr std::uint64_t fractional_units_per_second = 1ULL << 32U;
constexpr std::uint64_t compact_units_per_second = 1ULL << 16U;
constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000ULL;

}  // namespace

NtpTimestampResult ntp_timestamp(
    const std::chrono::system_clock::time_point time) {
    using Seconds = std::chrono::seconds;
    using Nanoseconds = std::chrono::nanoseconds;

    const auto since_unix_epoch =
        std::chrono::duration_cast<Nanoseconds>(time.time_since_epoch());
    const auto whole_seconds = std::chrono::floor<Seconds>(since_unix_epoch);
    const auto fractional_nanoseconds = since_unix_epoch - whole_seconds;
    const auto ntp_seconds = whole_seconds.count() +
                             ntp_unix_epoch_offset_seconds;
    if (ntp_seconds < 0 ||
        ntp_seconds >
            static_cast<std::int64_t>(
                std::numeric_limits<std::uint32_t>::max())) {
        return std::unexpected{
            "time is outside the representable NTP era"};
    }

    const auto fraction =
        static_cast<std::uint64_t>(fractional_nanoseconds.count()) *
        fractional_units_per_second / nanoseconds_per_second;
    return (static_cast<std::uint64_t>(ntp_seconds) << 32U) | fraction;
}

CompactNtpDurationResult compact_ntp_duration(
    const std::chrono::nanoseconds duration) {
    if (duration < std::chrono::nanoseconds::zero()) {
        return std::unexpected{
            "compact NTP duration must not be negative"};
    }

    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(duration);
    const auto remainder = duration - seconds;
    if (seconds.count() >
        static_cast<std::int64_t>(
            std::numeric_limits<std::uint16_t>::max())) {
        return std::unexpected{
            "duration exceeds compact NTP range"};
    }

    const auto fractional =
        static_cast<std::uint64_t>(remainder.count()) *
        compact_units_per_second / nanoseconds_per_second;
    return (static_cast<std::uint32_t>(seconds.count()) << 16U) |
           static_cast<std::uint32_t>(fractional);
}

std::chrono::nanoseconds duration_from_compact_ntp(
    const std::uint32_t duration) noexcept {
    const auto seconds = static_cast<std::uint64_t>(duration >> 16U);
    const auto fractional =
        static_cast<std::uint64_t>(duration & 0xffffU);
    const auto fractional_nanoseconds =
        fractional * nanoseconds_per_second / compact_units_per_second;
    return std::chrono::seconds{seconds} +
           std::chrono::nanoseconds{fractional_nanoseconds};
}

}  // namespace semilive::common::rtcp
