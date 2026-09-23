#include <semilive/publisher/reporting/publisher_session_report.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;
namespace reporting = semilive::publisher::reporting;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

reporting::PublisherSessionReport sample_report() {
    reporting::PublisherSessionReport report;
    report.run_succeeded = true;
    report.session_duration = 12'345ms;
    report.config.output.rtp_udp.destination_address = "127.0.0.1";
    report.config.output.rtp_udp.destination_port = 5004;
    report.config.rtcp.emplace();
    report.config.rtcp->transport.bind_address = "127.0.0.1";
    report.config.rtcp->transport.bind_port = 5005;
    report.config.rtcp->transport.peer_address = "127.0.0.1";
    report.config.rtcp->transport.peer_port = 5007;
    report.stats.session_id = 9;
    report.stats.started_sessions = 1;
    report.stats.completed_sessions = 1;
    report.stats.capture.published_frames = 300;
    report.stats.encoder.submitted_access_units = 299;
    report.stats.encoder.key_frames = 5;
    report.stats.encoder.encoded_bytes = 1'000'000;
    report.stats.output.consumed_access_units = 299;
    report.stats.output.input_bytes = 1'000'000;
    report.stats.output.emitted_units = 900;
    report.stats.output.emitted_bytes = 1'020'000;
    report.stats.rtcp.emplace();
    report.stats.rtcp->state =
        semilive::publisher::domain::PublisherRtcpWorkerState::Idle;
    report.stats.rtcp->transport = {
        "127.0.0.1", 5005, 1'500, 262'144};
    report.stats.rtcp->sender_reports_sent = 12;
    report.stats.rtcp->receiver_reports_received = 11;
    report.stats.rtcp->rtt_samples = 10;
    report.stats.rtcp->current_rtt = 17ms;
    report.stats.rtcp->reported_fraction_lost = 13;
    report.stats.rtcp->reported_cumulative_lost = 7;
    report.stats.rtcp->reported_jitter = 900;
    return report;
}

void renders_baseline_fields() {
    const auto json =
        reporting::render_publisher_session_report_json(sample_report());
    require(json.starts_with("{\n") && json.ends_with("}\n"),
            "publisher report must be a complete JSON object");
    require(json.find("\"schema_version\": 2") != std::string::npos &&
                json.find("\"application\": \"semilive_publisher\"") !=
                    std::string::npos &&
                json.find("\"duration_ms\": 12345") != std::string::npos &&
                json.find("\"target_bit_rate\": 4000000") !=
                    std::string::npos &&
                json.find("\"gop_size\": 60") != std::string::npos &&
                json.find("\"media_datagrams\": 900") !=
                    std::string::npos &&
                json.find("\"media_bytes\": 1020000") !=
                    std::string::npos,
            "publisher report must expose media and RTP baseline fields");
    require(json.find("\"bind_port\": 5005") != std::string::npos &&
                json.find("\"peer_port\": 5007") != std::string::npos &&
                json.find("\"sender_reports_sent\": 12") !=
                    std::string::npos &&
                json.find("\"receiver_reports_received\": 11") !=
                    std::string::npos &&
                json.find("\"current_rtt_ms\": 17") !=
                    std::string::npos &&
                json.find("\"reported_fraction_lost\": 13") !=
                    std::string::npos &&
                json.find("\"reported_fraction_lost_percent\": 5.07812") !=
                    std::string::npos &&
                json.find("\"reported_cumulative_lost\": 7") !=
                    std::string::npos &&
                json.find("\"reported_jitter_rtp_ticks\": 900") !=
                    std::string::npos,
            "publisher report must expose configured and observed RTCP data");
}

void renders_null_rtcp_when_disabled() {
    reporting::PublisherSessionReport report;
    const auto json =
        reporting::render_publisher_session_report_json(report);
    require(json.find("    \"rtcp\": null") != std::string::npos &&
                json.find("  \"rtcp\": null") != std::string::npos,
            "publisher report must distinguish disabled RTCP");
}

void writes_report() {
    const auto path = std::filesystem::temp_directory_path() /
                      "semilive_publisher_session_report_test.json";
    const auto report = sample_report();
    const auto written =
        reporting::write_publisher_session_report_json(path, report);
    require(written.has_value(), "publisher report must be written");
    std::ifstream input{path, std::ios::binary};
    const std::string contents{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
    require(contents == reporting::render_publisher_session_report_json(report),
            "written publisher report must match rendered JSON");
    std::error_code error;
    std::filesystem::remove(path, error);
}

}  // namespace

int main() {
    try {
        renders_baseline_fields();
        renders_null_rtcp_when_disabled();
        writes_report();
    } catch (const std::exception& error) {
        std::cerr << "publisher session report test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
