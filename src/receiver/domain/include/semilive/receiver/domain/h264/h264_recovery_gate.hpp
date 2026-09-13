#pragma once

#include <semilive/receiver/domain/h264/h264_access_unit_assembler.hpp>
#include <semilive/receiver/model/video/playable_h264_access_unit.hpp>

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
    H264RecoveryState state = H264RecoveryState::WaitingForRandomAccess;
};

class H264RecoveryGate final {
public:
    [[nodiscard]] std::optional<model::PlayableH264AccessUnit> consume(
        H264AccessUnitAssemblerEvent event);

    // Used when continuity is lost after assembly, for example when the
    // real-time output rejects an access unit.
    void require_random_access() noexcept;

    [[nodiscard]] H264RecoveryGateStats stats() const noexcept;
    [[nodiscard]] H264RecoveryState state() const noexcept;
    void reset() noexcept;

private:
    H264RecoveryState state_ = H264RecoveryState::WaitingForRandomAccess;
    H264RecoveryGateStats stats_;
};

}  // namespace semilive::receiver::domain
