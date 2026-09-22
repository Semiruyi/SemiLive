#pragma once

#include <semilive/publisher/contracts/output/rtp_sender_observer.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

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
    void begin_session(std::uint32_t ssrc) noexcept override;
    void record_sent_packet(
        std::uint32_t rtp_timestamp,
        std::size_t payload_octets,
        std::chrono::steady_clock::time_point sent_at) noexcept override;
    void end_session() noexcept override;

    [[nodiscard]] std::optional<RtpSenderSnapshot> snapshot() const noexcept;

private:
    mutable std::mutex mutex_;
    std::optional<RtpSenderSnapshot> snapshot_;
};

}  // namespace semilive::publisher::domain
