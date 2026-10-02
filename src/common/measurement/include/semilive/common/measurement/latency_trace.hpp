#pragma once

#include <cstdint>

namespace semilive::common::measurement {

struct FrameTiming {
    std::int64_t capture = 0;
    std::int64_t encode_begin = 0;
    std::int64_t encode_complete = 0;
};
struct LatencyRecord {
    std::uint32_t ssrc = 0;
    std::uint32_t rtp_timestamp = 0;
    std::uint64_t frame_id = 0;
    FrameTiming timing{};
    std::int64_t first_send = 0;
    std::int64_t last_send_complete = 0;
    std::int64_t first_receive = 0;
    std::int64_t last_receive = 0;
    std::int64_t au_ready = 0;
    std::uint64_t au_bytes = 0;
    std::uint64_t packet_count = 0;
    bool key_frame = false;
};

// QPC ticks are comparable only within the same Windows host and boot.
// Collection is opt-in; rows are exported on normal process exit.
[[nodiscard]] bool latency_enabled() noexcept;
[[nodiscard]] std::int64_t latency_ticks() noexcept;
void record_latency(const LatencyRecord& record) noexcept;
}  // namespace semilive::common::measurement
