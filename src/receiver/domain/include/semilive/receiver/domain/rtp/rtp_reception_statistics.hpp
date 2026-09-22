#pragma once

#include <semilive/common/rtcp/rtcp_packet.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace semilive::receiver::domain {

struct RtpReceptionStats {
    std::optional<std::uint32_t> source_ssrc;
    std::uint64_t received_packets = 0;
    std::uint64_t expected_packets = 0;
    std::int64_t cumulative_lost = 0;
    std::uint32_t extended_highest_sequence = 0;
    std::uint32_t interarrival_jitter = 0;
};

class RtpReceptionStatistics final {
public:
    void observe(std::uint32_t ssrc,
                 std::uint16_t sequence,
                 std::uint32_t timestamp,
                 std::chrono::steady_clock::time_point received_at) noexcept;

    [[nodiscard]] std::optional<common::rtcp::ReceptionReportBlock>
    take_report() noexcept;
    [[nodiscard]] RtpReceptionStats stats() const noexcept;
    void reset() noexcept;

private:
    [[nodiscard]] std::uint64_t expected_packets_locked() const noexcept;
    [[nodiscard]] std::uint32_t jitter_locked() const noexcept;

    mutable std::mutex mutex_;
    std::optional<std::uint32_t> source_ssrc_;
    std::uint16_t base_sequence_ = 0;
    std::uint16_t maximum_sequence_ = 0;
    std::uint32_t sequence_cycles_ = 0;
    std::uint64_t received_packets_ = 0;
    std::uint64_t prior_expected_packets_ = 0;
    std::uint64_t prior_received_packets_ = 0;
    std::optional<std::chrono::steady_clock::time_point> previous_arrival_;
    std::uint32_t previous_timestamp_ = 0;
    double jitter_ = 0.0;
};

}  // namespace semilive::receiver::domain
