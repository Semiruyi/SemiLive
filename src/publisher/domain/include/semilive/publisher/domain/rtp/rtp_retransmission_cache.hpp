#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace semilive::publisher::domain {

struct RtpRetransmissionCacheConfig {
    std::chrono::milliseconds retention{500};
    std::size_t maximum_packets = 2'048;
    std::size_t maximum_bytes = 4U * 1'024U * 1'024U;
};

struct RtpRetransmissionCacheStats {
    std::size_t current_packets = 0;
    std::size_t current_bytes = 0;
    std::uint64_t stored_packets = 0;
    std::uint64_t lookup_requests = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t cache_misses = 0;
    std::uint64_t expired_packets = 0;
    std::uint64_t capacity_evictions = 0;
    std::uint64_t replaced_packets = 0;
};

class RtpRetransmissionCache {
public:
    explicit RtpRetransmissionCache(
        RtpRetransmissionCacheConfig config = {});

    void store(std::uint16_t sequence_number,
               std::span<const std::byte> datagram,
               std::chrono::steady_clock::time_point sent_at);

    [[nodiscard]] std::optional<std::vector<std::byte>> find(
        std::uint16_t sequence_number,
        std::chrono::steady_clock::time_point now);

    [[nodiscard]] RtpRetransmissionCacheStats stats(
        std::chrono::steady_clock::time_point now);

    void clear() noexcept;

private:
    struct Entry {
        std::uint16_t sequence_number = 0;
        std::vector<std::byte> datagram;
        std::chrono::steady_clock::time_point sent_at;
    };

    using Entries = std::list<Entry>;

    void prune_expired(std::chrono::steady_clock::time_point now) noexcept;
    void erase(Entries::iterator entry) noexcept;

    RtpRetransmissionCacheConfig config_;
    std::mutex mutex_;
    Entries entries_;
    std::unordered_map<std::uint16_t, Entries::iterator> by_sequence_;
    RtpRetransmissionCacheStats stats_;
};

}  // namespace semilive::publisher::domain
