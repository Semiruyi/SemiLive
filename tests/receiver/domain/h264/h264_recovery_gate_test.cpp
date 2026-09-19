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

using Clock = domain::H264RecoveryGate::Clock;

[[nodiscard]] Clock::time_point at(const std::int64_t milliseconds) {
    return Clock::time_point{std::chrono::milliseconds{milliseconds}};
}

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
    require(!gate.consume(access_unit_event(1000, false, false, false),
                          at(10)),
            "ordinary AU must be dropped during startup");
    require(!gate.consume(access_unit_event(2000, true, false, false),
                          at(20)),
            "IDR without parameter sets must not recover playback");

    auto recovered = gate.consume(access_unit_event(3000, true, true, true),
                                  at(30));
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
        gate.consume(access_unit_event(4000, true, true, true), at(40)));

    auto ordinary = gate.consume(access_unit_event(5000, false, false, false),
                                 at(50));
    require(ordinary && !ordinary->discontinuity_before(),
            "ordinary AU must pass without a decoder refresh in streaming");

    auto periodic_idr =
        gate.consume(access_unit_event(6000, true, true, true), at(60));
    require(periodic_idr && !periodic_idr->discontinuity_before(),
            "healthy periodic IDR must not trigger a decoder refresh");
}

void assembler_discontinuity_waits_until_the_next_recovery_point() {
    domain::H264RecoveryGate gate;
    static_cast<void>(
        gate.consume(access_unit_event(7000, true, true, true), at(70)));

    require(!gate.consume(discontinuity_event(), at(80)),
            "discontinuity must be retained instead of emitted alone");
    require(gate.state() ==
                domain::H264RecoveryState::WaitingForRandomAccess,
            "assembler discontinuity must re-enter recovery mode");
    require(!gate.consume(access_unit_event(8000, false, false, false),
                          at(90)),
            "post-loss inter frame must be dropped");

    auto recovered = gate.consume(access_unit_event(9000, true, true, true),
                                  at(120));
    require(recovered && recovered->discontinuity_before(),
            "first complete random access AU after loss must carry refresh");

    const auto stats = gate.stats();
    require(stats.assembler_discontinuities == 1 &&
                stats.dropped_while_waiting == 1 &&
                stats.recovery_points == 2 &&
                stats.recovery_episodes_started == 1 &&
                stats.recovery_episodes_completed == 1 &&
                stats.recovery_wait_total == std::chrono::milliseconds{40} &&
                stats.recovery_wait_maximum ==
                    std::chrono::milliseconds{40} &&
                stats.delivered_access_units == 2,
            "recovery statistics must exclude startup and time post-loss cycles");
}

void external_output_loss_uses_the_same_recovery_policy() {
    domain::H264RecoveryGate gate;
    static_cast<void>(
        gate.consume(access_unit_event(10'000, true, true, true), at(10)));

    gate.require_random_access(at(20));
    gate.require_random_access(at(25));
    require(!gate.consume(access_unit_event(11'000, false, false, false),
                          at(30)),
            "output loss must suppress dependent frames");
    auto recovered =
        gate.consume(access_unit_event(12'000, true, true, true), at(50));
    require(recovered && recovered->discontinuity_before(),
            "output loss must recover with the same atomic refresh contract");
    const auto stats = gate.stats();
    require(stats.external_discontinuities == 2 &&
                stats.recovery_episodes_started == 1 &&
                stats.recovery_episodes_completed == 1 &&
                stats.recovery_wait_total == std::chrono::milliseconds{30},
            "repeated recovery requests must be observable without restarting timing");
}

void active_recovery_wait_is_visible_without_marking_completion() {
    domain::H264RecoveryGate gate;
    static_cast<void>(
        gate.consume(access_unit_event(12'500, true, true, true), at(10)));
    gate.require_random_access(at(20));

    const auto stats = gate.stats(at(75));
    require(stats.recovery_episodes_started == 1 &&
                stats.recovery_episodes_completed == 0 &&
                stats.recovery_wait_total == std::chrono::nanoseconds::zero() &&
                stats.active_recovery_wait == std::chrono::milliseconds{55} &&
                stats.recovery_wait_total_including_active ==
                    std::chrono::milliseconds{55} &&
                stats.recovery_wait_maximum_including_active ==
                    std::chrono::milliseconds{55},
            "an unfinished recovery must remain visible in a final snapshot");
}

void reset_restores_new_session_state_and_statistics() {
    domain::H264RecoveryGate gate;
    static_cast<void>(
        gate.consume(access_unit_event(13'000, true, true, true), at(10)));
    gate.require_random_access(at(20));
    gate.reset();

    const auto stats = gate.stats();
    require(stats.input_access_units == 0 &&
                stats.assembler_discontinuities == 0 &&
                stats.external_discontinuities == 0 &&
                stats.dropped_while_waiting == 0 &&
                stats.delivered_access_units == 0 &&
                stats.recovery_points == 0 &&
                stats.recovery_episodes_started == 0 &&
                stats.recovery_episodes_completed == 0 &&
                stats.recovery_wait_total == std::chrono::nanoseconds::zero(),
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
        active_recovery_wait_is_visible_without_marking_completion();
        reset_restores_new_session_state_and_statistics();
    } catch (const std::exception& error) {
        std::cerr << "H.264 recovery gate test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
