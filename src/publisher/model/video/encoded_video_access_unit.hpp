#pragma once

#include "publisher/model/media_time.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace semilive::publisher::model {

struct EncodedVideoAccessUnit {
    std::vector<std::byte> annex_b;
    MediaTime presentation_time{};
    bool key_frame = false;
    std::uint64_t source_sequence = 0;
    std::chrono::steady_clock::time_point captured_at{};
};

}  // namespace semilive::publisher::model
