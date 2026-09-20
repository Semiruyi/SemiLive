#include <semilive/relay/reporting/relay_session_report.hpp>

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <string_view>

namespace semilive::relay::reporting {
namespace {

void write_json_string(std::ostream& output, const std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output.put('"');
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (byte < 0x20U) {
                output << "\\u00" << hex[(byte >> 4U) & 0x0fU]
                       << hex[byte & 0x0fU];
            } else {
                output.put(character);
            }
            break;
        }
    }
    output.put('"');
}

std::int64_t milliseconds(const std::chrono::nanoseconds value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value)
        .count();
}

double configured_loss_percent(const std::uint32_t rate_ppm) {
    return static_cast<double>(rate_ppm) / 10'000.0;
}

double actual_loss_percent(
    const application::RelaySessionStats& stats) {
    if (stats.received_datagrams == 0) {
        return 0.0;
    }
    return 100.0 * static_cast<double>(stats.dropped_datagrams) /
           static_cast<double>(stats.received_datagrams);
}

}  // namespace

std::string render_relay_session_report_json(
    const RelaySessionReport& report) {
    const auto& config = report.config;
    const auto& stats = report.stats;

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(6)
           << "{\n"
              "  \"schema_version\": 1,\n"
              "  \"application\": \"semilive_relay\",\n"
              "  \"application_version\": \"0.1.0-dev\",\n"
              "  \"config\": {\n"
              "    \"input\": {\n"
              "      \"bind_address\": ";
    write_json_string(output, config.network.bind_address);
    output << ",\n"
              "      \"bind_port\": "
           << config.network.bind_port << ",\n"
              "      \"maximum_datagram_bytes\": "
           << config.network.maximum_datagram_bytes << ",\n"
              "      \"receive_buffer_bytes\": "
           << config.network.receive_buffer_bytes << "\n"
              "    },\n"
              "    \"output\": {\n"
              "      \"forward_address\": ";
    write_json_string(output, config.network.forward_address);
    output << ",\n"
              "      \"forward_port\": "
           << config.network.forward_port << "\n"
              "    },\n"
              "    \"impairment\": {\n"
              "      \"loss_percent\": "
           << configured_loss_percent(config.loss_rate_ppm) << ",\n"
              "      \"loss_rate_ppm\": "
           << config.loss_rate_ppm << ",\n"
              "      \"seed\": "
           << config.random_seed << "\n"
              "    },\n"
              "    \"poll_interval_ms\": "
           << config.poll_interval.count() << "\n"
              "  },\n"
              "  \"session\": {\n"
              "    \"run_succeeded\": "
           << (report.run_succeeded ? "true" : "false") << ",\n"
              "    \"duration_ms\": "
           << milliseconds(report.session_duration) << "\n"
              "  },\n"
              "  \"socket\": {\n"
              "    \"bound_address\": ";
    write_json_string(output, report.info.bound_address);
    output << ",\n"
              "    \"bound_port\": "
           << report.info.bound_port << ",\n"
              "    \"actual_receive_buffer_bytes\": "
           << report.info.receive_buffer_bytes << "\n"
              "  },\n"
              "  \"traffic\": {\n"
              "    \"received_datagrams\": "
           << stats.received_datagrams << ",\n"
              "    \"received_bytes\": "
           << stats.received_bytes << ",\n"
              "    \"forwarded_datagrams\": "
           << stats.forwarded_datagrams << ",\n"
              "    \"forwarded_bytes\": "
           << stats.forwarded_bytes << ",\n"
              "    \"dropped_datagrams\": "
           << stats.dropped_datagrams << ",\n"
              "    \"dropped_bytes\": "
           << stats.dropped_bytes << ",\n"
              "    \"actual_loss_percent\": "
           << actual_loss_percent(stats) << ",\n"
              "    \"receive_timeouts\": "
           << stats.receive_timeouts << ",\n"
              "    \"send_failures\": "
           << stats.send_failures << "\n"
              "  }\n"
              "}\n";
    return output.str();
}

RelaySessionReportWriteResult write_relay_session_report_json(
    const std::filesystem::path& path,
    const RelaySessionReport& report) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        return std::unexpected{
            "failed to open relay stats output: " + path.string()};
    }
    output << render_relay_session_report_json(report);
    output.flush();
    if (!output) {
        return std::unexpected{
            "failed to write relay stats output: " + path.string()};
    }
    return {};
}

}  // namespace semilive::relay::reporting
