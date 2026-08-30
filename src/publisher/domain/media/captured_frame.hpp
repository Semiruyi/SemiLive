#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace semilive::publisher::domain {

struct CapturedFrame {
    std::vector<std::byte> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point captured_at{};
};

}  // namespace semilive::publisher::domain
