#include <semilive/receiver/domain/rtp/rtp_reception_statistics.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace semilive::receiver::domain {
namespace {

constexpr std::uint32_t sequence_space = 65'536U;
constexpr std::uint16_t half_sequence_space = 0x8000U;
constexpr std::int64_t rtp_clock_rate = 90'000;
constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;
constexpr std::int64_t minimum_signed_24 = -8'388'608;
constexpr std::int64_t maximum_signed_24 = 8'388'607;

[[nodiscard]] std::int64_t duration_to_rtp_ticks(
    const std::chrono::steady_clock::duration duration) noexcept {
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    const auto seconds = nanoseconds / nanoseconds_per_second;
    const auto remainder = nanoseconds % nanoseconds_per_second;
    return seconds * rtp_clock_rate +
           remainder * rtp_clock_rate / nanoseconds_per_second;
}

}  // namespace

void RtpReceptionStatistics::observe(
    const std::uint32_t ssrc,
    const std::uint16_t sequence,
    const std::uint32_t timestamp,
    const std::chrono::steady_clock::time_point received_at) noexcept {
    std::lock_guard lock{mutex_};
    if (!source_ssrc_ || *source_ssrc_ != ssrc) {
        source_ssrc_ = ssrc;
        base_sequence_ = sequence;
        maximum_sequence_ = sequence;
        sequence_cycles_ = 0;
        received_packets_ = 0;
        prior_expected_packets_ = 0;
        prior_received_packets_ = 0;
        previous_arrival_.reset();
        previous_timestamp_ = timestamp;
        jitter_ = 0.0;
    } else {
        const auto forward = static_cast<std::uint16_t>(
            sequence - maximum_sequence_);
        if (forward != 0U && forward < half_sequence_space) {
            if (sequence < maximum_sequence_) {
                sequence_cycles_ += sequence_space;
            }
            maximum_sequence_ = sequence;
        }
    }

    ++received_packets_;
    if (previous_arrival_) {
        const auto arrival_delta =
            duration_to_rtp_ticks(received_at - *previous_arrival_);
        const auto timestamp_delta = static_cast<std::int32_t>(
            timestamp - previous_timestamp_);
        const auto variation = std::abs(
            static_cast<double>(arrival_delta - timestamp_delta));
        jitter_ += (variation - jitter_) / 16.0;
    }
    previous_arrival_ = received_at;
    previous_timestamp_ = timestamp;
}

std::optional<common::rtcp::ReceptionReportBlock>
RtpReceptionStatistics::take_report() noexcept {
    std::lock_guard lock{mutex_};
    if (!source_ssrc_) {
        return std::nullopt;
    }

    const auto expected = expected_packets_locked();
    const auto expected_interval = expected - prior_expected_packets_;
    const auto received_interval = received_packets_ - prior_received_packets_;
    const auto lost_interval =
        static_cast<std::int64_t>(expected_interval) -
        static_cast<std::int64_t>(received_interval);
    std::uint8_t fraction_lost = 0;
    if (expected_interval != 0U && lost_interval > 0) {
        const auto fraction =
            static_cast<std::uint64_t>(lost_interval) * 256U /
            expected_interval;
        fraction_lost = static_cast<std::uint8_t>(
            std::min<std::uint64_t>(fraction, 255U));
    }
    prior_expected_packets_ = expected;
    prior_received_packets_ = received_packets_;

    const auto cumulative = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(expected) -
            static_cast<std::int64_t>(received_packets_),
        minimum_signed_24, maximum_signed_24);
    return common::rtcp::ReceptionReportBlock{
        .source_ssrc = *source_ssrc_,
        .fraction_lost = fraction_lost,
        .cumulative_lost = static_cast<std::int32_t>(cumulative),
        .extended_highest_sequence = sequence_cycles_ + maximum_sequence_,
        .interarrival_jitter = jitter_locked(),
    };
}

RtpReceptionStats RtpReceptionStatistics::stats() const noexcept {
    std::lock_guard lock{mutex_};
    if (!source_ssrc_) {
        return {};
    }
    const auto expected = expected_packets_locked();
    return {
        .source_ssrc = source_ssrc_,
        .received_packets = received_packets_,
        .expected_packets = expected,
        .cumulative_lost = static_cast<std::int64_t>(expected) -
                           static_cast<std::int64_t>(received_packets_),
        .extended_highest_sequence = sequence_cycles_ + maximum_sequence_,
        .interarrival_jitter = jitter_locked(),
    };
}

void RtpReceptionStatistics::reset() noexcept {
    std::lock_guard lock{mutex_};
    source_ssrc_.reset();
    base_sequence_ = 0;
    maximum_sequence_ = 0;
    sequence_cycles_ = 0;
    received_packets_ = 0;
    prior_expected_packets_ = 0;
    prior_received_packets_ = 0;
    previous_arrival_.reset();
    previous_timestamp_ = 0;
    jitter_ = 0.0;
}

std::uint64_t RtpReceptionStatistics::expected_packets_locked() const noexcept {
    if (!source_ssrc_) {
        return 0;
    }
    return static_cast<std::uint64_t>(sequence_cycles_) +
           maximum_sequence_ - base_sequence_ + 1U;
}

std::uint32_t RtpReceptionStatistics::jitter_locked() const noexcept {
    return static_cast<std::uint32_t>(std::clamp(
        jitter_, 0.0,
        static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
}

}  // namespace semilive::receiver::domain
