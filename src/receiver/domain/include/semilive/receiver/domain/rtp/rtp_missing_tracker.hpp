#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace semilive::receiver::domain {

struct RtpMissingTrackerConfig {
    std::chrono::milliseconds initial_nack_delay{10};
    std::chrono::milliseconds retry_interval{20};
    std::uint8_t maximum_nack_attempts = 2;
    // Keeps a worst-case uncompressed PID/BLP request below a 1200-byte MTU.
    std::size_t maximum_pending_packets = 256;
};

using RtpMissingTrackerConfigValidationResult =
    std::expected<void, std::string>;

[[nodiscard]] RtpMissingTrackerConfigValidationResult
validate_rtp_missing_tracker_config(const RtpMissingTrackerConfig& config);

struct RtpMissingTrackerStats {
    std::optional<std::uint32_t> source_ssrc;
    std::uint64_t observed_packets = 0;
    std::uint64_t detected_missing_packets = 0;
    std::uint64_t recovered_before_nack = 0;
    std::uint64_t recovered_after_nack = 0;
    std::uint64_t nack_batches = 0;
    std::uint64_t nack_sequence_requests = 0;
    std::uint64_t nack_retry_requests = 0;
    std::uint64_t exhausted_packets = 0;
    std::uint64_t abandoned_packets = 0;
    std::uint64_t capacity_ignored_packets = 0;
    std::size_t pending_packets = 0;
    std::size_t peak_pending_packets = 0;
};

class RtpMissingTracker final {
public:
    using Clock = std::chrono::steady_clock;

    explicit RtpMissingTracker(RtpMissingTrackerConfig config = {});

    void observe(std::uint32_t ssrc,
                 std::uint16_t sequence_number,
                 Clock::time_point received_at) noexcept;

    [[nodiscard]] std::vector<std::uint16_t> take_due_nacks(
        Clock::time_point now);

    void abandon(std::uint16_t first_missing,
                 std::uint16_t missing_count) noexcept;

    [[nodiscard]] RtpMissingTrackerStats stats() const noexcept;
    void reset() noexcept;

private:
    struct PendingPacket {
        Clock::time_point next_nack_at;
        std::uint8_t nack_attempts = 0;
        std::uint64_t detection_order = 0;
    };

    using PendingPackets = std::map<std::uint16_t, PendingPacket>;

    void reset_locked() noexcept;
    [[nodiscard]] Clock::time_point observe_time_locked(
        Clock::time_point now) noexcept;

    RtpMissingTrackerConfig config_;
    mutable std::mutex mutex_;
    std::optional<std::uint32_t> source_ssrc_;
    std::optional<std::uint16_t> highest_sequence_;
    std::optional<Clock::time_point> observed_time_;
    PendingPackets pending_;
    std::uint64_t next_detection_order_ = 0;
    RtpMissingTrackerStats stats_;
};

}  // namespace semilive::receiver::domain
