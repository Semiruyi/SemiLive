#pragma once

#include <semilive/receiver/model/media_time.hpp>
#include <semilive/receiver/model/video/h264_access_unit.hpp>

#include <cstdint>
#include <utility>

namespace semilive::receiver::model {

class TimedH264AccessUnit final {
public:
    TimedH264AccessUnit(H264AccessUnit access_unit,
                        const MediaTime presentation_time,
                        const std::uint64_t extended_rtp_timestamp,
                        const bool discontinuity_before) noexcept
        : access_unit_{std::move(access_unit)},
          presentation_time_{presentation_time},
          extended_rtp_timestamp_{extended_rtp_timestamp},
          discontinuity_before_{discontinuity_before} {}

    [[nodiscard]] const H264AccessUnit& access_unit() const noexcept {
        return access_unit_;
    }

    [[nodiscard]] H264AccessUnit take_access_unit() && noexcept {
        return std::move(access_unit_);
    }

    [[nodiscard]] MediaTime presentation_time() const noexcept {
        return presentation_time_;
    }

    [[nodiscard]] std::uint64_t extended_rtp_timestamp() const noexcept {
        return extended_rtp_timestamp_;
    }

    [[nodiscard]] bool discontinuity_before() const noexcept {
        return discontinuity_before_;
    }

private:
    H264AccessUnit access_unit_;
    MediaTime presentation_time_{};
    std::uint64_t extended_rtp_timestamp_ = 0;
    bool discontinuity_before_ = false;
};

}  // namespace semilive::receiver::model
