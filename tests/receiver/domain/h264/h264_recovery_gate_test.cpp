#include <semilive/receiver/domain/h264/h264_recovery_gate.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace domain = semilive::receiver::domain;
namespace model = semilive::receiver::model;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

[[nodiscard]] domain::H264AccessUnitAssemblerEvent access_unit_event(
    const std::uint32_t timestamp,
    const bool contains_idr,
    const bool contains_sps,
    const bool contains_pps) {
    return model::H264AccessUnit{
        std::vector<std::byte>{std::byte{0x00}, std::byte{0x00},
                               std::byte{0x00}, std::byte{0x01},
                               static_cast<std::byte>(
                                   contains_idr ? 0x65 : 0x61)},
        timestamp,
        10,
        10,
        model::H264AccessUnit::Clock::time_point{
            std::chrono::milliseconds{timestamp}},
        1,
        true,
        contains_idr,
        contains_sps,
        contains_pps};
}

[[nodiscard]] domain::H264AccessUnitAssemblerEvent discontinuity_event() {
    return domain::H264AccessUnitDiscontinuity{
        domain::H264AccessUnitDiscontinuityCode::NalSequenceMismatch,
        std::nullopt,
        9000};
}

void startup_waits_for_a_complete_random_access_candidate() {
    domain::H264RecoveryGate gate;

    require(gate.state() ==
                domain::H264RecoveryState::WaitingForRandomAccess,
            "gate must start in recovery mode");
    require(!gate.consume(access_unit_event(1000, false, false, false)),
            "ordinary AU must be dropped during startup");
    require(!gate.consume(access_unit_event(2000, true, false, false)),
            "IDR without parameter sets must not recover playback");

    auto recovered = gate.consume(access_unit_event(3000, true, true, true));
    require(recovered.has_value(),
            "SPS/PPS/IDR AU must recover playback");
    require(recovered->discontinuity_before(),
            "first playable AU must request decoder refresh");
    require(recovered->access_unit().rtp_timestamp() == 3000,
            "gate must preserve the accepted AU");
    require(gate.state() == domain::H264RecoveryState::Streaming,
            "accepted random access point must enter streaming state");
}

void streaming_passes_complete_access_units_without_spurious_resets() {
    domain::H264RecoveryGate gate;
    static_cast<void>(
        gate.consume(access_unit_event(4000, true, true, true)));

    auto ordinary = gate.consume(access_unit_event(5000, false, false, false));
    require(ordinary && !ordinary->discontinuity_before(),
            "ordinary AU must pass without a decoder refresh in streaming");

    auto periodic_idr =
        gate.consume(access_unit_event(6000, true, true, true));
    require(periodic_idr && !periodic_idr->discontinuity_before(),
            "healthy periodic IDR must not trigger a decoder refresh");
}

void assembler_discontinuity_waits_until_the_next_recovery_point() {
    domain::H264RecoveryGate gate;
    static_cast<void>(
        gate.consume(access_unit_event(7000, true, true, true)));

    require(!gate.consume(discontinuity_event()),
            "discontinuity must be retained instead of emitted alone");
    require(gate.state() ==
                domain::H264RecoveryState::WaitingForRandomAccess,
            "assembler discontinuity must re-enter recovery mode");
    require(!gate.consume(access_unit_event(8000, false, false, false)),
            "post-loss inter frame must be dropped");

    auto recovered = gate.consume(access_unit_event(9000, true, true, true));
    require(recovered && recovered->discontinuity_before(),
            "first complete random access AU after loss must carry refresh");

    const auto stats = gate.stats();
    require(stats.assembler_discontinuities == 1 &&
                stats.dropped_while_waiting == 1 &&
                stats.recovery_points == 2 &&
                stats.delivered_access_units == 2,
            "recovery statistics must include startup and post-loss cycles");
}

void external_output_loss_uses_the_same_recovery_policy() {
    domain::H264RecoveryGate gate;
    static_cast<void>(
        gate.consume(access_unit_event(10'000, true, true, true)));

    gate.require_random_access();
    require(!gate.consume(access_unit_event(11'000, false, false, false)),
            "output loss must suppress dependent frames");
    auto recovered =
        gate.consume(access_unit_event(12'000, true, true, true));
    require(recovered && recovered->discontinuity_before(),
            "output loss must recover with the same atomic refresh contract");
    require(gate.stats().external_discontinuities == 1,
            "external recovery requests must be observable");
}

void reset_restores_new_session_state_and_statistics() {
    domain::H264RecoveryGate gate;
    static_cast<void>(
        gate.consume(access_unit_event(13'000, true, true, true)));
    gate.require_random_access();
    gate.reset();

    const auto stats = gate.stats();
    require(stats.input_access_units == 0 &&
                stats.assembler_discontinuities == 0 &&
                stats.external_discontinuities == 0 &&
                stats.dropped_while_waiting == 0 &&
                stats.delivered_access_units == 0 &&
                stats.recovery_points == 0,
            "reset must clear per-session statistics");
    require(stats.state ==
                domain::H264RecoveryState::WaitingForRandomAccess,
            "reset must make a new session wait for random access");
}

}  // namespace

int main() {
    try {
        startup_waits_for_a_complete_random_access_candidate();
        streaming_passes_complete_access_units_without_spurious_resets();
        assembler_discontinuity_waits_until_the_next_recovery_point();
        external_output_loss_uses_the_same_recovery_policy();
        reset_restores_new_session_state_and_statistics();
    } catch (const std::exception& error) {
        std::cerr << "H.264 recovery gate test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
