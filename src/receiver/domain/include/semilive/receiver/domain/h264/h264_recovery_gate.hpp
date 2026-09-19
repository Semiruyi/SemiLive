#pragma once

#include <semilive/receiver/domain/h264/h264_access_unit_assembler.hpp>
#include <semilive/receiver/model/video/playable_h264_access_unit.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace semilive::receiver::domain {

enum class H264RecoveryState : std::uint8_t {
    WaitingForRandomAccess,
    Streaming,
};

struct H264RecoveryGateStats {
    std::uint64_t input_access_units = 0;
    std::uint64_t assembler_discontinuities = 0;
    std::uint64_t external_discontinuities = 0;
    std::uint64_t dropped_while_waiting = 0;
    std::uint64_t delivered_access_units = 0;
    std::uint64_t recovery_points = 0;
    std::uint64_t recovery_episodes_started = 0;
    std::uint64_t recovery_episodes_completed = 0;
    std::chrono::nanoseconds recovery_wait_total{};
    std::chrono::nanoseconds recovery_wait_maximum{};
    H264RecoveryState state = H264RecoveryState::WaitingForRandomAccess;
};

class H264RecoveryGate final {
public:
    using Clock = model::H264AccessUnit::Clock;

    [[nodiscard]] std::optional<model::PlayableH264AccessUnit> consume(
        H264AccessUnitAssemblerEvent event,
        Clock::time_point observed_at);

    // Used when continuity is lost after assembly, for example when the
    // real-time output rejects an access unit.
    void require_random_access(Clock::time_point observed_at) noexcept;

    [[nodiscard]] H264RecoveryGateStats stats() const noexcept;
    [[nodiscard]] H264RecoveryState state() const noexcept;
    void reset() noexcept;

private:
    void enter_recovery(Clock::time_point observed_at) noexcept;

    H264RecoveryState state_ = H264RecoveryState::WaitingForRandomAccess;
    H264RecoveryGateStats stats_;
    bool has_streamed_ = false;
    std::optional<Clock::time_point> recovery_started_at_;
};

}  // namespace semilive::receiver::domain
