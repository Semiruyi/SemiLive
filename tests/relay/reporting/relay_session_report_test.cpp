#include <semilive/relay/reporting/relay_session_report.hpp>

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

namespace {

using namespace std::chrono_literals;
namespace reporting = semilive::relay::reporting;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

reporting::RelaySessionReport sample_report() {
    reporting::RelaySessionReport report;
    report.run_succeeded = true;
    report.session_duration = 12'345ms;
    report.config.network.bind_address = "127.0.0.1";
    report.config.network.bind_port = 5004;
    report.config.network.forward_address = "127.0.0.1";
    report.config.network.forward_port = 5006;
    report.config.loss_rate_ppm = 10'000;
    report.config.random_seed = 20260919;
    report.info.bound_address = "127.0.0.1";
    report.info.bound_port = 5004;
    report.info.receive_buffer_bytes = 4U * 1024U * 1024U;
    report.stats.received_datagrams = 1'000;
    report.stats.received_bytes = 1'200'000;
    report.stats.forwarded_datagrams = 990;
    report.stats.forwarded_bytes = 1'188'000;
    report.stats.dropped_datagrams = 10;
    report.stats.dropped_bytes = 12'000;
    return report;
}

void renders_machine_readable_fields() {
    const auto json =
        reporting::render_relay_session_report_json(sample_report());
    require(json.starts_with("{\n") && json.ends_with("}\n"),
            "relay report must be a complete JSON object");
    require(json.find("\"application\": \"semilive_relay\"") !=
                    std::string::npos &&
                json.find("\"loss_percent\": 1.000000") !=
                    std::string::npos &&
                json.find("\"seed\": 20260919") != std::string::npos &&
                json.find("\"dropped_datagrams\": 10") !=
                    std::string::npos &&
                json.find("\"actual_loss_percent\": 1.000000") !=
                    std::string::npos,
            "relay report must expose impairment and observed loss fields");
}

void writes_report() {
    const auto path = std::filesystem::temp_directory_path() /
                      "semilive_relay_session_report_test.json";
    const auto report = sample_report();
    const auto written =
        reporting::write_relay_session_report_json(path, report);
    require(written.has_value(), "relay report must be written");
    std::ifstream input{path, std::ios::binary};
    const std::string contents{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
    require(contents == reporting::render_relay_session_report_json(report),
            "written relay report must match rendered JSON");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

}  // namespace

int main() {
    try {
        renders_machine_readable_fields();
        writes_report();
    } catch (const std::exception& error) {
        std::cerr << "relay session report test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
