#include <semilive/receiver/domain/h264/h264_recovery_gate.hpp>

#include <utility>
#include <variant>

namespace semilive::receiver::domain {

std::optional<model::PlayableH264AccessUnit> H264RecoveryGate::consume(
    H264AccessUnitAssemblerEvent event) {
    if (std::holds_alternative<H264AccessUnitDiscontinuity>(event)) {
        ++stats_.assembler_discontinuities;
        state_ = H264RecoveryState::WaitingForRandomAccess;
        return std::nullopt;
    }

    ++stats_.input_access_units;
    auto access_unit = std::get<model::H264AccessUnit>(std::move(event));
    if (state_ == H264RecoveryState::WaitingForRandomAccess) {
        if (!access_unit.is_random_access_candidate()) {
            ++stats_.dropped_while_waiting;
            return std::nullopt;
        }

        state_ = H264RecoveryState::Streaming;
        ++stats_.recovery_points;
        ++stats_.delivered_access_units;
        return model::PlayableH264AccessUnit{std::move(access_unit), true};
    }

    ++stats_.delivered_access_units;
    return model::PlayableH264AccessUnit{std::move(access_unit), false};
}

void H264RecoveryGate::require_random_access() noexcept {
    ++stats_.external_discontinuities;
    state_ = H264RecoveryState::WaitingForRandomAccess;
}

H264RecoveryGateStats H264RecoveryGate::stats() const noexcept {
    auto snapshot = stats_;
    snapshot.state = state_;
    return snapshot;
}

H264RecoveryState H264RecoveryGate::state() const noexcept {
    return state_;
}

void H264RecoveryGate::reset() noexcept {
    state_ = H264RecoveryState::WaitingForRandomAccess;
    stats_ = {};
}

}  // namespace semilive::receiver::domain
