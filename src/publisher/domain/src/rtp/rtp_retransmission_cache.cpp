#include <semilive/publisher/domain/rtp/rtp_retransmission_cache.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace semilive::publisher::domain {

RtpRetransmissionCache::RtpRetransmissionCache(
    RtpRetransmissionCacheConfig config)
    : config_{config} {
    if (config_.retention <= std::chrono::milliseconds::zero() ||
        config_.maximum_packets == 0U || config_.maximum_bytes == 0U) {
        throw std::invalid_argument{
            "RTP retransmission cache limits must be positive"};
    }
}

void RtpRetransmissionCache::store(
    const std::uint16_t sequence_number,
    const std::span<const std::byte> datagram,
    const std::chrono::steady_clock::time_point sent_at) {
    std::lock_guard lock{mutex_};
    prune_expired(sent_at);

    if (const auto existing = by_sequence_.find(sequence_number);
        existing != by_sequence_.end()) {
        erase(existing->second);
        ++stats_.replaced_packets;
    }
    if (datagram.empty() || datagram.size() > config_.maximum_bytes) {
        return;
    }

    entries_.push_back(
        Entry{sequence_number,
              std::vector<std::byte>{datagram.begin(), datagram.end()},
              sent_at});
    try {
        by_sequence_.emplace(sequence_number, std::prev(entries_.end()));
    } catch (...) {
        entries_.pop_back();
        throw;
    }
    stats_.current_bytes += datagram.size();
    ++stats_.stored_packets;

    while (entries_.size() > config_.maximum_packets ||
           stats_.current_bytes > config_.maximum_bytes) {
        erase(entries_.begin());
        ++stats_.capacity_evictions;
    }
    stats_.current_packets = entries_.size();
}

std::optional<std::vector<std::byte>> RtpRetransmissionCache::find(
    const std::uint16_t sequence_number,
    const std::chrono::steady_clock::time_point now) {
    std::lock_guard lock{mutex_};
    prune_expired(now);
    ++stats_.lookup_requests;
    const auto found = by_sequence_.find(sequence_number);
    if (found == by_sequence_.end()) {
        ++stats_.cache_misses;
        return std::nullopt;
    }
    ++stats_.cache_hits;
    return found->second->datagram;
}

RtpRetransmissionCacheStats RtpRetransmissionCache::stats(
    const std::chrono::steady_clock::time_point now) {
    std::lock_guard lock{mutex_};
    prune_expired(now);
    return stats_;
}

void RtpRetransmissionCache::clear() noexcept {
    std::lock_guard lock{mutex_};
    entries_.clear();
    by_sequence_.clear();
    stats_ = {};
}

void RtpRetransmissionCache::prune_expired(
    const std::chrono::steady_clock::time_point now) noexcept {
    while (!entries_.empty() && now >= entries_.front().sent_at &&
           now - entries_.front().sent_at >= config_.retention) {
        erase(entries_.begin());
        ++stats_.expired_packets;
    }
    stats_.current_packets = entries_.size();
}

void RtpRetransmissionCache::erase(const Entries::iterator entry) noexcept {
    stats_.current_bytes -= entry->datagram.size();
    by_sequence_.erase(entry->sequence_number);
    entries_.erase(entry);
    stats_.current_packets = entries_.size();
}

}  // namespace semilive::publisher::domain
