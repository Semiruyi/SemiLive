#pragma once

#include "publisher/domain/timing/session_timeline.hpp"
#include "publisher/model/media_time.hpp"
#include "publisher/model/video/frame_rate.hpp"

#include <chrono>
#include <cstdint>

namespace semilive::publisher::domain {

struct FrameTick {
    std::uint64_t schedule_index = 0;
    std::chrono::steady_clock::time_point deadline{};
    model::MediaTime presentation_time{};
};

struct FrameSchedule {
    FrameTick tick;
    std::uint64_t skipped_ticks = 0;
};

class FrameScheduler final {
public:
    using Clock = std::chrono::steady_clock;

    FrameScheduler(SessionTimeline timeline,
                   model::FrameRate frame_rate,
                   Clock::time_point track_start);

    [[nodiscard]] FrameTick initial_tick() const;
    [[nodiscard]] FrameSchedule advance_after(Clock::time_point now);

private:
    [[nodiscard]] model::MediaTime frame_offset(
        std::uint64_t schedule_index) const;
    [[nodiscard]] FrameTick make_tick(std::uint64_t schedule_index) const;
    [[nodiscard]] bool can_represent(std::uint64_t schedule_index) const noexcept;
    [[nodiscard]] std::uint64_t find_maximum_schedule_index() const noexcept;

    SessionTimeline timeline_;
    model::FrameRate frame_rate_;
    Clock::time_point track_start_;
    model::MediaTime first_presentation_time_{};
    std::uint64_t nanoseconds_per_rate_numerator_ = 0;
    std::uint64_t current_schedule_index_ = 0;
    std::uint64_t maximum_schedule_index_ = 0;
};

}  // namespace semilive::publisher::domain
