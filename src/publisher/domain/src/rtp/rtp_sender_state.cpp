#include <semilive/publisher/domain/rtp/rtp_sender_state.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace semilive::publisher::domain {

RtpSenderState::RtpSenderState(RtpRetransmissionCacheConfig cache_config)
    : retransmission_cache_{std::move(cache_config)} {}

void RtpSenderState::begin_session(const std::uint32_t ssrc) noexcept {
    std::lock_guard lock{mutex_};
    retransmission_cache_.clear();
    snapshot_ = RtpSenderSnapshot{
        .ssrc = ssrc,
        .last_rtp_timestamp = 0,
        .last_sent_at = {},
        .packet_count = 0,
        .payload_octet_count = 0,
        .has_sent_packet = false,
    };
}

void RtpSenderState::record_sent_packet(
    const std::uint16_t sequence_number,
    const std::uint32_t rtp_timestamp,
    const std::size_t payload_octets,
    const std::span<const std::byte> datagram,
    const std::chrono::steady_clock::time_point sent_at) noexcept {
    std::lock_guard lock{mutex_};
    if (!snapshot_) {
        return;
    }
    snapshot_->last_rtp_timestamp = rtp_timestamp;
    snapshot_->last_sent_at = sent_at;
    snapshot_->packet_count += 1U;
    const auto bounded_octets = std::min<std::size_t>(
        payload_octets, std::numeric_limits<std::uint32_t>::max());
    snapshot_->payload_octet_count +=
        static_cast<std::uint32_t>(bounded_octets);
    snapshot_->has_sent_packet = true;
    try {
        retransmission_cache_.store(sequence_number, datagram, sent_at);
    } catch (...) {
        // Media delivery must continue if an optional cache allocation fails.
    }
}

void RtpSenderState::end_session() noexcept {
    std::lock_guard lock{mutex_};
    snapshot_.reset();
    retransmission_cache_.clear();
}

std::optional<RtpSenderSnapshot> RtpSenderState::snapshot() const noexcept {
    std::lock_guard lock{mutex_};
    return snapshot_;
}

std::optional<std::vector<std::byte>>
RtpSenderState::find_retransmission_packet(
    const std::uint16_t sequence_number,
    const std::chrono::steady_clock::time_point now) {
    std::lock_guard lock{mutex_};
    if (!snapshot_) {
        return std::nullopt;
    }
    return retransmission_cache_.find(sequence_number, now);
}

RtpRetransmissionCacheStats RtpSenderState::retransmission_cache_stats(
    const std::chrono::steady_clock::time_point now) {
    std::lock_guard lock{mutex_};
    return retransmission_cache_.stats(now);
}

}  // namespace semilive::publisher::domain
