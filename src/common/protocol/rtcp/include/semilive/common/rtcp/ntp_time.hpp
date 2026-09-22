#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>

namespace semilive::common::rtcp {

using NtpTimestampResult = std::expected<std::uint64_t, std::string>;
using CompactNtpDurationResult =
    std::expected<std::uint32_t, std::string>;

[[nodiscard]] NtpTimestampResult ntp_timestamp(
    std::chrono::system_clock::time_point time);

[[nodiscard]] constexpr std::uint32_t compact_ntp(
    const std::uint64_t timestamp) noexcept {
    return static_cast<std::uint32_t>((timestamp >> 16U) & 0xffff'ffffULL);
}

[[nodiscard]] CompactNtpDurationResult compact_ntp_duration(
    std::chrono::nanoseconds duration);

[[nodiscard]] std::chrono::nanoseconds duration_from_compact_ntp(
    std::uint32_t duration) noexcept;

}  // namespace semilive::common::rtcp
