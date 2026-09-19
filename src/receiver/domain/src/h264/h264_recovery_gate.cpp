#include <semilive/receiver/domain/h264/h264_recovery_gate.hpp>

#include <algorithm>
#include <chrono>
#include <utility>
#include <variant>

namespace semilive::receiver::domain {

std::optional<model::PlayableH264AccessUnit> H264RecoveryGate::consume(
    H264AccessUnitAssemblerEvent event,
    const Clock::time_point observed_at) {
    if (std::holds_alternative<H264AccessUnitDiscontinuity>(event)) {
        ++stats_.assembler_discontinuities;
        enter_recovery(observed_at);
        return std::nullopt;
    }

    ++stats_.input_access_units;
    auto access_unit = std::get<model::H264AccessUnit>(std::move(event));
    if (state_ == H264RecoveryState::WaitingForRandomAccess) {
        if (!access_unit.is_random_access_candidate()) {
            ++stats_.dropped_while_waiting;
            return std::nullopt;
        }

        if (recovery_started_at_) {
            const auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::max(Clock::duration::zero(),
                         observed_at - *recovery_started_at_));
            ++stats_.recovery_episodes_completed;
            stats_.recovery_wait_total += wait;
            stats_.recovery_wait_maximum =
                std::max(stats_.recovery_wait_maximum, wait);
            recovery_started_at_.reset();
        }

        state_ = H264RecoveryState::Streaming;
        has_streamed_ = true;
        ++stats_.recovery_points;
        ++stats_.delivered_access_units;
        return model::PlayableH264AccessUnit{std::move(access_unit), true};
    }

    ++stats_.delivered_access_units;
    return model::PlayableH264AccessUnit{std::move(access_unit), false};
}

void H264RecoveryGate::require_random_access(
    const Clock::time_point observed_at) noexcept {
    ++stats_.external_discontinuities;
    enter_recovery(observed_at);
}

void H264RecoveryGate::enter_recovery(
    const Clock::time_point observed_at) noexcept {
    if (state_ == H264RecoveryState::Streaming && has_streamed_) {
        ++stats_.recovery_episodes_started;
        recovery_started_at_ = observed_at;
    }
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
    has_streamed_ = false;
    recovery_started_at_.reset();
}

}  // namespace semilive::receiver::domain
