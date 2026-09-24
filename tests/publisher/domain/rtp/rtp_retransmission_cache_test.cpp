#include <semilive/publisher/domain/rtp/rtp_retransmission_cache.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace domain = semilive::publisher::domain;
using Clock = std::chrono::steady_clock;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

[[nodiscard]] std::vector<std::byte> bytes(
    const std::initializer_list<std::uint8_t> values) {
    std::vector<std::byte> result;
    for (const auto value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

void returns_complete_datagrams_by_wrapping_sequence_number() {
    domain::RtpRetransmissionCache cache;
    const auto start = Clock::time_point{};
    const auto first = bytes({0x80, 0x60, 0xff, 0xff, 0x01});
    const auto second = bytes({0x80, 0x60, 0x00, 0x00, 0x02});
    cache.store(65'535U, first, start);
    cache.store(0U, second, start + 1ms);

    require(cache.find(65'535U, start + 10ms) == first &&
                cache.find(0U, start + 10ms) == second,
            "cache did not preserve complete datagrams across RTP wraparound");
    const auto stats = cache.stats(start + 10ms);
    require(stats.current_packets == 2U && stats.current_bytes == 10U &&
                stats.lookup_requests == 2U && stats.cache_hits == 2U,
            "cache did not report stored packets and lookup hits");
}

void expires_packets_at_the_retention_boundary() {
    domain::RtpRetransmissionCache cache{{50ms, 10U, 1'000U}};
    const auto start = Clock::time_point{};
    cache.store(7U, bytes({1, 2, 3}), start);

    require(cache.find(7U, start + 49ms).has_value(),
            "cache expired a packet before its retention deadline");
    require(!cache.find(7U, start + 50ms),
            "cache retained a packet at its expiration deadline");
    const auto stats = cache.stats(start + 50ms);
    require(stats.current_packets == 0U && stats.expired_packets == 1U &&
                stats.cache_hits == 1U && stats.cache_misses == 1U,
            "cache expiration or lookup counters are incorrect");
}

void enforces_packet_and_byte_limits() {
    const auto start = Clock::time_point{};
    domain::RtpRetransmissionCache packet_limited{{1s, 2U, 1'000U}};
    packet_limited.store(1U, bytes({1}), start);
    packet_limited.store(2U, bytes({2}), start + 1ms);
    packet_limited.store(3U, bytes({3}), start + 2ms);
    require(!packet_limited.find(1U, start + 3ms) &&
                packet_limited.find(2U, start + 3ms) &&
                packet_limited.find(3U, start + 3ms),
            "packet limit did not evict the oldest datagram");

    domain::RtpRetransmissionCache byte_limited{{1s, 10U, 5U}};
    byte_limited.store(10U, bytes({1, 2, 3}), start);
    byte_limited.store(11U, bytes({4, 5, 6}), start + 1ms);
    const auto stats = byte_limited.stats(start + 2ms);
    require(!byte_limited.find(10U, start + 2ms) &&
                byte_limited.find(11U, start + 2ms) &&
                stats.current_packets == 1U && stats.current_bytes == 3U &&
                stats.capacity_evictions == 1U,
            "byte limit did not evict the oldest datagram");
}

void replaces_reused_sequence_numbers_and_clears_session_state() {
    domain::RtpRetransmissionCache cache;
    const auto start = Clock::time_point{};
    cache.store(42U, bytes({1}), start);
    cache.store(42U, bytes({2, 3}), start + 1ms);
    require(cache.find(42U, start + 2ms) == bytes({2, 3}),
            "cache did not replace a reused RTP sequence number");
    require(cache.stats(start + 2ms).replaced_packets == 1U,
            "cache did not count a replaced sequence number");

    cache.clear();
    const auto cleared = cache.stats(start + 2ms);
    require(cleared.current_packets == 0U && cleared.stored_packets == 0U &&
                !cache.find(42U, start + 2ms),
            "cache clear did not reset packet and metric state");
}

void rejects_non_positive_limits() {
    bool rejected = false;
    try {
        domain::RtpRetransmissionCache cache{{0ms, 1U, 1U}};
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "cache accepted a non-positive retention duration");
}

}  // namespace

int main() {
    try {
        returns_complete_datagrams_by_wrapping_sequence_number();
        expires_packets_at_the_retention_boundary();
        enforces_packet_and_byte_limits();
        replaces_reused_sequence_numbers_and_clears_session_state();
        rejects_non_positive_limits();
        std::cout << "RTP retransmission cache tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "RTP retransmission cache test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
