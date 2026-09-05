#pragma once

#include "publisher/model/media_time.hpp"

#include <chrono>

namespace semilive::publisher::domain {

class SessionTimeline final {
public:
    using Clock = std::chrono::steady_clock;

    explicit SessionTimeline(Clock::time_point origin) noexcept;

    [[nodiscard]] Clock::time_point origin() const noexcept;
    [[nodiscard]] model::MediaTime media_time_at(
        Clock::time_point presentation_time) const noexcept;

private:
    Clock::time_point origin_;
};

}  // namespace semilive::publisher::domain
