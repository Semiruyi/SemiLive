#pragma once

#include <semilive/publisher/contracts/output/rtp_sender_observer.hpp>
#include <semilive/publisher/domain/rtp/rtp_retransmission_cache.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace semilive::publisher::domain {

struct RtpSenderSnapshot {
    std::uint32_t ssrc = 0;
    std::uint32_t last_rtp_timestamp = 0;
    std::chrono::steady_clock::time_point last_sent_at;
    std::uint32_t packet_count = 0;
    std::uint32_t payload_octet_count = 0;
    bool has_sent_packet = false;
};

class RtpSenderState final : public contracts::output::RtpSenderObserver {
public:
    explicit RtpSenderState(RtpRetransmissionCacheConfig cache_config = {});

    void begin_session(std::uint32_t ssrc) noexcept override;
    void record_sent_packet(
        std::uint16_t sequence_number,
        std::uint32_t rtp_timestamp,
        std::size_t payload_octets,
        std::span<const std::byte> datagram,
        std::chrono::steady_clock::time_point sent_at) noexcept override;
    void end_session() noexcept override;

    [[nodiscard]] std::optional<RtpSenderSnapshot> snapshot() const noexcept;
    [[nodiscard]] std::optional<std::vector<std::byte>>
    find_retransmission_packet(
        std::uint16_t sequence_number,
        std::chrono::steady_clock::time_point now);
    [[nodiscard]] RtpRetransmissionCacheStats retransmission_cache_stats(
        std::chrono::steady_clock::time_point now);

private:
    mutable std::mutex mutex_;
    std::optional<RtpSenderSnapshot> snapshot_;
    RtpRetransmissionCache retransmission_cache_;
};

}  // namespace semilive::publisher::domain
