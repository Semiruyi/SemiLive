#include "publisher/domain/timing/session_timeline.hpp"

namespace semilive::publisher::domain {

SessionTimeline::SessionTimeline(const Clock::time_point origin) noexcept : origin_{origin} {}

SessionTimeline::Clock::time_point SessionTimeline::origin() const noexcept {
    return origin_;
}

model::MediaTime SessionTimeline::media_time_at(
    const Clock::time_point presentation_time) const noexcept {
    return std::chrono::duration_cast<model::MediaTime>(presentation_time - origin_);
}

}  // namespace semilive::publisher::domain
