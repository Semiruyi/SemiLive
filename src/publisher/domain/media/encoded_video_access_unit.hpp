#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace semilive::publisher::domain {

struct EncodedVideoAccessUnit {
    std::vector<std::byte> annex_b;
    std::int64_t pts_90khz = 0;
    bool key_frame = false;
    std::uint64_t source_sequence = 0;
    std::chrono::steady_clock::time_point captured_at{};
};

}  // namespace semilive::publisher::domain
