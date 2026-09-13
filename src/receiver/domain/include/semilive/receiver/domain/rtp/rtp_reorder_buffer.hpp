#pragma once

#include <semilive/receiver/model/rtp/rtp_packet.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace semilive::receiver::domain {

struct RtpReorderConfig {
    std::size_t maximum_buffered_packets = 64;
    std::chrono::milliseconds maximum_hold_time{50};
};

using RtpReorderConfigValidationResult = std::expected<void, std::string>;

[[nodiscard]] RtpReorderConfigValidationResult
validate_rtp_reorder_config(const RtpReorderConfig& config);

enum class RtpSequenceGapCause : std::uint8_t {
    HoldTimeout,
    BufferCapacity,
};

struct OrderedRtpPacket {
    model::RtpPacket packet;
};

struct RtpSequenceGap {
    std::uint16_t first_missing = 0;
    std::uint16_t next_received = 0;
    std::uint16_t missing_count = 0;
    RtpSequenceGapCause cause = RtpSequenceGapCause::HoldTimeout;
};

using RtpReorderEvent = std::variant<OrderedRtpPacket, RtpSequenceGap>;
using RtpReorderEvents = std::vector<RtpReorderEvent>;

struct RtpReorderStats {
    std::uint64_t received_packets = 0;
    std::uint64_t ordered_packets = 0;
    std::uint64_t reordered_packets = 0;
    std::uint64_t duplicate_packets = 0;
    std::uint64_t late_packets = 0;
    std::uint64_t confirmed_gaps = 0;
    std::uint64_t confirmed_lost_packets = 0;
    std::uint64_t timeout_gaps = 0;
    std::uint64_t capacity_gaps = 0;
    std::size_t buffered_packets = 0;
    std::size_t peak_buffered_packets = 0;
};

class RtpReorderBuffer final {
public:
    using Clock = model::RtpPacket::Clock;

    explicit RtpReorderBuffer(RtpReorderConfig config = {});
    ~RtpReorderBuffer();

    RtpReorderBuffer(const RtpReorderBuffer&) = delete;
    RtpReorderBuffer& operator=(const RtpReorderBuffer&) = delete;
    RtpReorderBuffer(RtpReorderBuffer&&) noexcept;
    RtpReorderBuffer& operator=(RtpReorderBuffer&&) noexcept;

    [[nodiscard]] RtpReorderEvents push(model::RtpPacket packet);
    [[nodiscard]] RtpReorderEvents poll(Clock::time_point now);

    [[nodiscard]] RtpReorderStats stats() const noexcept;
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::receiver::domain
