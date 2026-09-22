#include <semilive/receiver/domain/rtp/rtp_reception_statistics.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using semilive::receiver::domain::RtpReceptionStatistics;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

void reports_interval_loss_and_sequence_wrap() {
    RtpReceptionStatistics statistics;
    const auto started = std::chrono::steady_clock::time_point{};
    statistics.observe(0x1122'3344U, 65'534U, 0U, started);
    statistics.observe(0x1122'3344U, 65'535U, 3'000U, started + 33ms);
    statistics.observe(0x1122'3344U, 1U, 9'000U, started + 100ms);

    const auto first = statistics.take_report();
    require(first.has_value(), "RTP reception report was not produced");
    require(first->source_ssrc == 0x1122'3344U &&
                first->extended_highest_sequence == 65'537U &&
                first->cumulative_lost == 1 &&
                first->fraction_lost == 64U,
            "RTP reception report did not account for sequence wrap and loss");

    statistics.observe(0x1122'3344U, 2U, 12'000U, started + 133ms);
    const auto second = statistics.take_report();
    require(second.has_value() && second->fraction_lost == 0U &&
                second->cumulative_lost == 1,
            "RTP interval loss was not reset after a report");
}

void measures_interarrival_jitter_in_rtp_ticks() {
    RtpReceptionStatistics statistics;
    const auto started = std::chrono::steady_clock::time_point{};
    statistics.observe(7U, 10U, 0U, started);
    statistics.observe(7U, 11U, 900U, started + 20ms);
    const auto snapshot = statistics.stats();
    require(snapshot.interarrival_jitter == 56U,
            "RTP interarrival jitter did not use the RFC 3550 estimator");

    statistics.reset();
    require(!statistics.take_report(),
            "reset RTP reception statistics retained a source");
}

}  // namespace

int main() {
    try {
        reports_interval_loss_and_sequence_wrap();
        measures_interarrival_jitter_in_rtp_ticks();
        std::cout << "RTP reception statistics tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "RTP reception statistics test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
