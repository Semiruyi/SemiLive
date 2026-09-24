#include <semilive/receiver/domain/rtp/rtp_missing_tracker.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace domain = semilive::receiver::domain;
using Clock = domain::RtpMissingTracker::Clock;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

void schedules_initial_nack_and_one_retry() {
    domain::RtpMissingTracker tracker;
    const auto start = Clock::time_point{};
    tracker.observe(7U, 100U, start);
    tracker.observe(7U, 102U, start + 1ms);

    require(tracker.take_due_nacks(start + 10ms).empty(),
            "missing packet became due before its initial NACK delay");
    require(tracker.take_due_nacks(start + 11ms) ==
                std::vector<std::uint16_t>{101U},
            "missing packet did not become due after 10ms");
    require(tracker.take_due_nacks(start + 30ms).empty(),
            "NACK retry became due before its retry interval");
    require(tracker.take_due_nacks(start + 31ms) ==
                std::vector<std::uint16_t>{101U},
            "missing packet did not receive its one retry");
    require(tracker.take_due_nacks(start + 100ms).empty(),
            "missing packet exceeded the configured NACK attempt limit");

    const auto stats = tracker.stats();
    require(stats.detected_missing_packets == 1U &&
                stats.nack_batches == 2U &&
                stats.nack_sequence_requests == 2U &&
                stats.nack_retry_requests == 1U &&
                stats.exhausted_packets == 1U &&
                stats.pending_packets == 1U,
            "NACK scheduling counters are incorrect");
}

void distinguishes_recovery_before_and_after_nack() {
    domain::RtpMissingTracker tracker;
    const auto start = Clock::time_point{};
    tracker.observe(9U, 200U, start);
    tracker.observe(9U, 202U, start + 1ms);
    tracker.observe(9U, 201U, start + 5ms);

    tracker.observe(9U, 204U, start + 6ms);
    require(tracker.take_due_nacks(start + 16ms) ==
                std::vector<std::uint16_t>{203U},
            "second missing packet did not become due");
    tracker.observe(9U, 203U, start + 20ms);

    const auto stats = tracker.stats();
    require(stats.recovered_before_nack == 1U &&
                stats.recovered_after_nack == 1U &&
                stats.pending_packets == 0U,
            "missing packet recovery phase was not classified correctly");
}

void tracks_gaps_across_sequence_wraparound() {
    domain::RtpMissingTracker tracker;
    const auto start = Clock::time_point{};
    tracker.observe(11U, 65'534U, start);
    tracker.observe(11U, 1U, start + 1ms);

    require(tracker.take_due_nacks(start + 11ms) ==
                std::vector<std::uint16_t>{65'535U, 0U},
            "missing packets across RTP wraparound were not tracked");
    tracker.observe(11U, 65'535U, start + 12ms);
    tracker.observe(11U, 0U, start + 13ms);
    require(tracker.stats().recovered_after_nack == 2U,
            "wrapped missing packets were not recovered");
}

void abandons_confirmed_gaps_and_bounds_pending_packets() {
    domain::RtpMissingTracker tracker{{10ms, 20ms, 2U, 2U}};
    const auto start = Clock::time_point{};
    tracker.observe(13U, 10U, start);
    tracker.observe(13U, 14U, start + 1ms);
    auto stats = tracker.stats();
    require(stats.pending_packets == 2U &&
                stats.capacity_ignored_packets == 1U,
            "missing tracker did not enforce its pending packet bound");

    tracker.abandon(11U, 3U);
    stats = tracker.stats();
    require(stats.pending_packets == 0U && stats.abandoned_packets == 2U &&
                tracker.take_due_nacks(start + 100ms).empty(),
            "confirmed gap remained eligible for NACK");
}

void resets_tracking_when_the_source_changes() {
    domain::RtpMissingTracker tracker;
    const auto start = Clock::time_point{};
    tracker.observe(1U, 10U, start);
    tracker.observe(1U, 12U, start + 1ms);
    tracker.observe(2U, 50U, start + 2ms);

    const auto stats = tracker.stats();
    require(stats.source_ssrc == 2U && stats.observed_packets == 1U &&
                stats.detected_missing_packets == 0U &&
                stats.pending_packets == 0U,
            "new RTP source inherited the previous source's missing state");
}

void rejects_invalid_configuration() {
    require(!domain::validate_rtp_missing_tracker_config(
                {-1ms, 20ms, 2U, 512U}) &&
                !domain::validate_rtp_missing_tracker_config(
                    {10ms, 0ms, 2U, 512U}) &&
                !domain::validate_rtp_missing_tracker_config(
                    {10ms, 20ms, 0U, 512U}) &&
                !domain::validate_rtp_missing_tracker_config(
                    {10ms, 20ms, 2U, 0U}),
            "missing tracker accepted invalid timing or capacity bounds");
}

}  // namespace

int main() {
    try {
        schedules_initial_nack_and_one_retry();
        distinguishes_recovery_before_and_after_nack();
        tracks_gaps_across_sequence_wraparound();
        abandons_confirmed_gaps_and_bounds_pending_packets();
        resets_tracking_when_the_source_changes();
        rejects_invalid_configuration();
        std::cout << "RTP missing tracker tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "RTP missing tracker test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
