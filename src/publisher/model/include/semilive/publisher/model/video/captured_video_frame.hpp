#pragma once
#include <semilive/common/measurement/latency_trace.hpp>

#include <semilive/publisher/model/media_time.hpp>
#include <semilive/publisher/model/video/bgra_frame_buffer.hpp>

#include <chrono>
#include <cstdint>

namespace semilive::publisher::model {

struct CapturedVideoFrame {
    SharedBgraFrameBuffer image;
    std::uint64_t sequence = 0;
    MediaTime presentation_time{};
    std::chrono::steady_clock::time_point captured_at{};
    common::measurement::FrameTiming timing{};
};

}  // namespace semilive::publisher::model
