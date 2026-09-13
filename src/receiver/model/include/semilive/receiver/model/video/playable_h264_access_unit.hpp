#pragma once

#include <semilive/receiver/model/video/h264_access_unit.hpp>

#include <utility>

namespace semilive::receiver::model {

class PlayableH264AccessUnit final {
public:
    PlayableH264AccessUnit(H264AccessUnit access_unit,
                          const bool discontinuity_before) noexcept
        : access_unit_{std::move(access_unit)},
          discontinuity_before_{discontinuity_before} {}

    [[nodiscard]] const H264AccessUnit& access_unit() const noexcept {
        return access_unit_;
    }

    [[nodiscard]] H264AccessUnit take_access_unit() && noexcept {
        return std::move(access_unit_);
    }

    [[nodiscard]] bool discontinuity_before() const noexcept {
        return discontinuity_before_;
    }

private:
    H264AccessUnit access_unit_;
    bool discontinuity_before_ = false;
};

}  // namespace semilive::receiver::model
