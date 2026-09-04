#pragma once

#include "publisher/domain/media/bgra_frame_buffer.hpp"
#include "publisher/domain/timing/media_time.hpp"

#include <chrono>
#include <cstdint>

namespace semilive::publisher::domain {

struct CapturedVideoFrame {
    SharedBgraFrameBuffer image;
    std::uint64_t sequence = 0;
    MediaTime presentation_time{};
    std::chrono::steady_clock::time_point captured_at{};
};

}  // namespace semilive::publisher::domain
