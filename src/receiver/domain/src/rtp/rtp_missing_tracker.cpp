#include <semilive/receiver/domain/rtp/rtp_missing_tracker.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace semilive::receiver::domain {
namespace {

constexpr std::uint16_t half_sequence_space = 0x8000U;

}  // namespace

RtpMissingTrackerConfigValidationResult validate_rtp_missing_tracker_config(
    const RtpMissingTrackerConfig& config) {
    if (config.initial_nack_delay < std::chrono::milliseconds::zero()) {
        return std::unexpected{
            "RTP initial NACK delay must not be negative"};
    }
    if (config.retry_interval <= std::chrono::milliseconds::zero()) {
        return std::unexpected{"RTP NACK retry interval must be positive"};
    }
    if (config.maximum_nack_attempts == 0U) {
        return std::unexpected{"RTP maximum NACK attempts must be positive"};
    }
    if (config.maximum_pending_packets == 0U ||
        config.maximum_pending_packets >= half_sequence_space) {
        return std::unexpected{
            "RTP maximum pending packets must be in 1..32767"};
    }
    return {};
}

RtpMissingTracker::RtpMissingTracker(RtpMissingTrackerConfig config)
    : config_{config} {
    const auto valid = validate_rtp_missing_tracker_config(config_);
    if (!valid) {
        throw std::invalid_argument{valid.error()};
    }
}

void RtpMissingTracker::observe(
    const std::uint32_t ssrc,
    const std::uint16_t sequence_number,
    const Clock::time_point received_at) noexcept {
    std::lock_guard lock{mutex_};
    const auto now = observe_time_locked(received_at);

    if (!source_ssrc_ || *source_ssrc_ != ssrc) {
        reset_locked();
        source_ssrc_ = ssrc;
        highest_sequence_ = sequence_number;
        observed_time_ = now;
        stats_.source_ssrc = ssrc;
        stats_.observed_packets = 1U;
        return;
    }

    ++stats_.observed_packets;
    if (const auto missing = pending_.find(sequence_number);
        missing != pending_.end()) {
        if (missing->second.nack_attempts == 0U) {
            ++stats_.recovered_before_nack;
        } else {
            ++stats_.recovered_after_nack;
        }
        pending_.erase(missing);
        stats_.pending_packets = pending_.size();
    }

    if (!highest_sequence_) {
        highest_sequence_ = sequence_number;
        return;
    }
    const auto forward = static_cast<std::uint16_t>(
        sequence_number - *highest_sequence_);
    if (forward == 0U || forward >= half_sequence_space) {
        return;
    }

    const auto gap_size = static_cast<std::size_t>(forward - 1U);
    const auto available = config_.maximum_pending_packets -
                           std::min(config_.maximum_pending_packets,
                                    pending_.size());
    const auto tracked = std::min(gap_size, available);
    std::size_t recorded = 0;
    for (std::size_t offset = 1U; offset <= tracked; ++offset) {
        const auto sequence = static_cast<std::uint16_t>(
            *highest_sequence_ + static_cast<std::uint16_t>(offset));
        try {
            if (pending_.emplace(
                    sequence,
                    PendingPacket{now + config_.initial_nack_delay, 0U,
                                  next_detection_order_})
                    .second) {
                ++next_detection_order_;
                ++recorded;
                ++stats_.detected_missing_packets;
            }
        } catch (...) {
            break;
        }
    }
    stats_.capacity_ignored_packets += gap_size - recorded;
    stats_.pending_packets = pending_.size();
    stats_.peak_pending_packets =
        std::max(stats_.peak_pending_packets, pending_.size());
    highest_sequence_ = sequence_number;
}

std::vector<std::uint16_t> RtpMissingTracker::take_due_nacks(
    const Clock::time_point now) {
    std::lock_guard lock{mutex_};
    const auto observed_now = observe_time_locked(now);
    std::vector<std::pair<std::uint64_t, std::uint16_t>> ordered_due;
    ordered_due.reserve(pending_.size());
    for (auto& [sequence, packet] : pending_) {
        if (packet.nack_attempts >= config_.maximum_nack_attempts ||
            observed_now < packet.next_nack_at) {
            continue;
        }
        ordered_due.emplace_back(packet.detection_order, sequence);
        if (packet.nack_attempts > 0U) {
            ++stats_.nack_retry_requests;
        }
        ++packet.nack_attempts;
        ++stats_.nack_sequence_requests;
        packet.next_nack_at = observed_now + config_.retry_interval;
        if (packet.nack_attempts == config_.maximum_nack_attempts) {
            ++stats_.exhausted_packets;
        }
    }
    if (!ordered_due.empty()) {
        ++stats_.nack_batches;
    }
    std::sort(ordered_due.begin(), ordered_due.end());
    std::vector<std::uint16_t> due;
    due.reserve(ordered_due.size());
    for (const auto& [order, sequence] : ordered_due) {
        static_cast<void>(order);
        due.push_back(sequence);
    }
    return due;
}

void RtpMissingTracker::abandon(
    const std::uint16_t first_missing,
    const std::uint16_t missing_count) noexcept {
    std::lock_guard lock{mutex_};
    for (std::uint32_t offset = 0; offset < missing_count; ++offset) {
        const auto sequence = static_cast<std::uint16_t>(
            first_missing + static_cast<std::uint16_t>(offset));
        stats_.abandoned_packets += pending_.erase(sequence);
    }
    stats_.pending_packets = pending_.size();
}

RtpMissingTrackerStats RtpMissingTracker::stats() const noexcept {
    std::lock_guard lock{mutex_};
    return stats_;
}

void RtpMissingTracker::reset() noexcept {
    std::lock_guard lock{mutex_};
    reset_locked();
}

void RtpMissingTracker::reset_locked() noexcept {
    source_ssrc_.reset();
    highest_sequence_.reset();
    observed_time_.reset();
    pending_.clear();
    next_detection_order_ = 0;
    stats_ = {};
}

RtpMissingTracker::Clock::time_point
RtpMissingTracker::observe_time_locked(const Clock::time_point now) noexcept {
    if (!observed_time_ || now > *observed_time_) {
        observed_time_ = now;
    }
    return *observed_time_;
}

}  // namespace semilive::receiver::domain
