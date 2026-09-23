#include <semilive/receiver/reporting/receiver_session_report.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using namespace std::chrono_literals;

namespace reporting = semilive::receiver::reporting;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

class TemporaryFile final {
public:
    explicit TemporaryFile(std::string suffix) {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::current_path() /
                ("semilive_receiver_report_" + std::to_string(nonce) +
                 std::move(suffix));
    }

    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] reporting::ReceiverSessionReport sample_report() {
    reporting::ReceiverSessionReport report;
    report.run_succeeded = true;
    report.config.input.bind_address = "127.0.0.1";
    report.config.input.bind_port = 5004;
    report.config.pipeline.session.ssrc = 0x1234'5678U;
    report.config.h264_output_path = "video\"baseline.h264";
    report.config.rtcp.emplace();
    report.config.rtcp->transport.bind_address = "127.0.0.1";
    report.config.rtcp->transport.bind_port = 5007;
    report.config.rtcp->transport.peer_address = "127.0.0.1";
    report.config.rtcp->transport.peer_port = 5005;
    report.stats.session_id = 7;
    report.stats.session_duration = 1'234ms;
    report.stats.first_output_delay = 45ms;
    report.stats.maximum_output_gap = 67ms;
    report.stats.terminal_output_gap = 89ms;
    report.stats.output_stall_events = 2;
    report.stats.output_stall_excess_total = 40ms;
    report.stats.received_datagrams = 100;
    report.stats.received_bytes = 120'000;
    report.stats.pipeline.reorder.confirmed_lost_packets = 3;
    report.stats.pipeline.assembler.discarded_access_units = 2;
    report.stats.pipeline.recovery.recovery_episodes_started = 2;
    report.stats.pipeline.recovery.recovery_episodes_completed = 1;
    report.stats.pipeline.recovery.recovery_wait_total = 300ms;
    report.stats.pipeline.recovery.recovery_wait_maximum = 300ms;
    report.stats.pipeline.recovery.active_recovery_wait = 125ms;
    report.stats.pipeline.recovery.recovery_wait_total_including_active = 425ms;
    report.stats.pipeline.recovery.recovery_wait_maximum_including_active = 300ms;
    report.stats.submitted_access_units = 80;
    report.stats.submitted_bytes = 12'345;
    report.stats.rtcp.emplace();
    report.stats.rtcp->state =
        semilive::receiver::domain::ReceiverRtcpWorkerState::Idle;
    report.stats.rtcp->transport = {
        "127.0.0.1", 5007, 1'500, 262'144};
    report.stats.rtcp->sender_reports_received = 12;
    report.stats.rtcp->receiver_reports_sent = 11;
    report.stats.rtcp->current_fraction_lost = 13;
    report.stats.rtcp->cumulative_lost = 7;
    report.stats.rtcp->extended_highest_sequence = 65'540;
    report.stats.rtcp->interarrival_jitter = 900;
    report.stats.rtcp->last_sender_report = 0x1234'5678U;
    report.stats.rtcp->delay_since_last_sender_report = 65'536;
    return report;
}

void renders_machine_readable_baseline_fields() {
    const auto json =
        reporting::render_receiver_session_report_json(sample_report());
    require(json.starts_with("{\n") && json.ends_with("}\n"),
            "report must be a complete JSON object");
    require(json.find("\"schema_version\": 3") != std::string::npos &&
                json.find("\"mode\": \"file\"") != std::string::npos &&
                json.find("\"run_succeeded\": true") !=
                    std::string::npos &&
                json.find("\"duration_ms\": 1234") !=
                    std::string::npos &&
                json.find("\"received_bytes\": 120000") !=
                    std::string::npos &&
                json.find("\"first_output_delay_ms\": 45") !=
                    std::string::npos &&
                json.find("\"terminal_output_gap_ms\": 89") !=
                    std::string::npos &&
                json.find("\"output_stall_events\": 2") !=
                    std::string::npos &&
                json.find("\"confirmed_lost_packets\": 3") !=
                    std::string::npos &&
                json.find("\"discarded_access_units\": 2") !=
                    std::string::npos &&
                json.find("\"episodes_completed\": 1") !=
                    std::string::npos &&
                json.find("\"active_wait_ms\": 125") !=
                    std::string::npos &&
                json.find("\"wait_total_including_active_ms\": 425") !=
                    std::string::npos,
            "report must expose baseline transport and recovery metrics");
    require(json.find("\"bind_port\": 5007") != std::string::npos &&
                json.find("\"peer_port\": 5005") != std::string::npos &&
                json.find("\"sender_reports_received\": 12") !=
                    std::string::npos &&
                json.find("\"receiver_reports_sent\": 11") !=
                    std::string::npos &&
                json.find("\"current_fraction_lost\": 13") !=
                    std::string::npos &&
                json.find("\"current_fraction_lost_percent\": 5.07812") !=
                    std::string::npos &&
                json.find("\"cumulative_lost\": 7") !=
                    std::string::npos &&
                json.find("\"extended_highest_sequence\": 65540") !=
                    std::string::npos &&
                json.find("\"interarrival_jitter_rtp_ticks\": 900") !=
                    std::string::npos,
            "receiver report must expose configured and observed RTCP data");
    require(json.find("video\\\"baseline.h264") != std::string::npos,
            "report must JSON-escape configured paths");
}

void renders_null_rtcp_when_disabled() {
    reporting::ReceiverSessionReport report;
    const auto json =
        reporting::render_receiver_session_report_json(report);
    require(json.find("    \"rtcp\": null") != std::string::npos &&
                json.find("  \"rtcp\": null") != std::string::npos,
            "receiver report must distinguish disabled RTCP");
}

void writes_the_rendered_report_and_reports_open_failures() {
    TemporaryFile output{".json"};
    const auto report = sample_report();
    const auto written = reporting::write_receiver_session_report_json(
        output.path(), report);
    require(written.has_value(), "report file must be written");

    std::ifstream input{output.path(), std::ios::binary};
    const std::string contents{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
    require(contents ==
                reporting::render_receiver_session_report_json(report),
            "written report must equal the rendered JSON");

    const auto invalid_path =
        output.path() / "missing-parent" / "report.json";
    const auto failed = reporting::write_receiver_session_report_json(
        invalid_path, report);
    require(!failed &&
                failed.error().find("failed to open") != std::string::npos,
            "report writer must return a structured open failure");
}

}  // namespace

int main() {
    try {
        renders_machine_readable_baseline_fields();
        renders_null_rtcp_when_disabled();
        writes_the_rendered_report_and_reports_open_failures();
    } catch (const std::exception& error) {
        std::cerr << "receiver session report test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
