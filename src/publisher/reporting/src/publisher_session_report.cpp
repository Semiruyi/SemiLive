#include <semilive/publisher/reporting/publisher_session_report.hpp>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <locale>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace semilive::publisher::reporting {
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
    return std::chrono::duration_cast<std::chrono::milliseconds>(value).count();
}

template <typename Integer>
void write_optional_integer(std::ostream& output,
                            const std::optional<Integer>& value) {
    if (!value) {
        output << "null";
        return;
    }
    output << *value;
}

void write_optional_byte(std::ostream& output,
                         const std::optional<std::uint8_t>& value) {
    if (!value) {
        output << "null";
        return;
    }
    output << static_cast<unsigned int>(*value);
}

void write_optional_fraction_lost_percent(
    std::ostream& output,
    const std::optional<std::uint8_t>& value) {
    if (!value) {
        output << "null";
        return;
    }
    output << (100.0 * static_cast<double>(*value) / 256.0);
}

void write_optional_duration(
    std::ostream& output,
    const std::optional<std::chrono::nanoseconds>& value) {
    if (!value) {
        output << "null";
        return;
    }
    output << milliseconds(*value);
}

std::string_view state_name(
    const application::PublisherControllerState state) noexcept {
    using State = application::PublisherControllerState;
    switch (state) {
    case State::Idle:
        return "idle";
    case State::Starting:
        return "starting";
    case State::Running:
        return "running";
    case State::Stopping:
        return "stopping";
    case State::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view selection_name(
    const contracts::capture::DesktopOutputSelection selection) noexcept {
    using Selection = contracts::capture::DesktopOutputSelection;
    switch (selection) {
    case Selection::Primary:
        return "primary";
    case Selection::Index:
        return "index";
    }
    return "unknown";
}

std::string_view rtcp_state_name(
    const domain::PublisherRtcpWorkerState state) noexcept {
    using State = domain::PublisherRtcpWorkerState;
    switch (state) {
    case State::Idle:
        return "idle";
    case State::Running:
        return "running";
    case State::Failed:
        return "failed";
    }
    return "unknown";
}

std::string path_text(const std::filesystem::path& path) {
    return path.generic_string();
}

void write_rtcp_config(std::ostream& output,
                       const composition::PublisherConfig& config) {
    output << "    \"rtcp\": ";
    if (!config.rtcp) {
        output << "null\n";
        return;
    }

    const auto& rtcp = *config.rtcp;
    output << "{\n"
              "      \"bind_address\": ";
    write_json_string(output, rtcp.transport.bind_address);
    output << ",\n"
              "      \"bind_port\": "
           << rtcp.transport.bind_port << ",\n"
              "      \"peer_address\": ";
    write_json_string(output, rtcp.transport.peer_address);
    output << ",\n"
              "      \"peer_port\": "
           << rtcp.transport.peer_port << ",\n"
              "      \"maximum_datagram_bytes\": "
           << rtcp.transport.maximum_datagram_bytes << ",\n"
              "      \"receive_buffer_bytes\": "
           << rtcp.transport.receive_buffer_bytes << ",\n"
              "      \"report_interval_ms\": "
           << rtcp.report_interval.count() << "\n"
              "    }\n";
}

void write_rtcp_stats(std::ostream& output,
                      const application::PublisherControllerStats& stats) {
    output << "  \"rtcp\": ";
    if (!stats.rtcp) {
        output << "null\n";
        return;
    }

    const auto& rtcp = *stats.rtcp;
    output << "{\n"
              "    \"state\": ";
    write_json_string(output, rtcp_state_name(rtcp.state));
    output << ",\n"
              "    \"bound_address\": ";
    if (rtcp.transport) {
        write_json_string(output, rtcp.transport->bound_address);
    } else {
        output << "null";
    }
    output << ",\n"
              "    \"bound_port\": ";
    if (rtcp.transport) {
        output << rtcp.transport->bound_port;
    } else {
        output << "null";
    }
    output << ",\n"
              "    \"actual_receive_buffer_bytes\": ";
    if (rtcp.transport) {
        output << rtcp.transport->receive_buffer_bytes;
    } else {
        output << "null";
    }
    output << ",\n"
              "    \"sender_reports_sent\": "
           << rtcp.sender_reports_sent << ",\n"
              "    \"receiver_reports_received\": "
           << rtcp.receiver_reports_received << ",\n"
              "    \"invalid_packets\": "
           << rtcp.invalid_packets << ",\n"
              "    \"ignored_report_blocks\": "
           << rtcp.ignored_report_blocks << ",\n"
              "    \"rtt_samples\": "
           << rtcp.rtt_samples << ",\n"
              "    \"current_rtt_ms\": ";
    write_optional_duration(output, rtcp.current_rtt);
    output << ",\n"
              "    \"reported_fraction_lost\": ";
    write_optional_byte(output, rtcp.reported_fraction_lost);
    output << ",\n"
              "    \"reported_fraction_lost_percent\": ";
    write_optional_fraction_lost_percent(
        output, rtcp.reported_fraction_lost);
    output << ",\n"
              "    \"reported_cumulative_lost\": ";
    write_optional_integer(output, rtcp.reported_cumulative_lost);
    output << ",\n"
              "    \"reported_jitter_rtp_ticks\": ";
    write_optional_integer(output, rtcp.reported_jitter);
    output << "\n"
              "  }\n";
}

}  // namespace

std::string render_publisher_session_report_json(
    const PublisherSessionReport& report) {
    const auto& config = report.config;
    const auto& stats = report.stats;
    const auto& encoder = config.video.encoder;
    const auto& rtp = config.output.rtp_udp;

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\n"
              "  \"schema_version\": 2,\n"
              "  \"application\": \"semilive_publisher\",\n"
              "  \"application_version\": \"0.1.0-dev\",\n"
              "  \"config\": {\n"
              "    \"capture\": {\n"
              "      \"display_selection\": ";
    write_json_string(output,
                      selection_name(config.video.capture.output.selection));
    output << ",\n"
              "      \"display_index\": "
           << config.video.capture.output.index << ",\n"
              "      \"compose_pointer\": "
           << (config.video.capture.compose_pointer ? "true" : "false")
           << "\n"
              "    },\n"
              "    \"video\": {\n"
              "      \"width\": "
           << encoder.output.width << ",\n"
              "      \"height\": "
           << encoder.output.height << ",\n"
              "      \"frame_rate_numerator\": "
           << encoder.frame_rate.numerator << ",\n"
              "      \"frame_rate_denominator\": "
           << encoder.frame_rate.denominator << ",\n"
              "      \"target_bit_rate\": "
           << encoder.target_bit_rate << ",\n"
              "      \"gop_size\": "
           << encoder.gop_size << "\n"
              "    },\n"
              "    \"rtp\": {\n"
              "      \"destination_address\": ";
    write_json_string(output, rtp.destination_address);
    output << ",\n"
              "      \"destination_port\": "
           << rtp.destination_port << ",\n"
              "      \"payload_type\": "
           << static_cast<unsigned int>(rtp.payload_type) << ",\n"
              "      \"maximum_datagram_bytes\": "
           << rtp.max_datagram_bytes << "\n"
              "    },\n";
    write_rtcp_config(output, config);
    output << "  },\n"
              "  \"session\": {\n"
              "    \"id\": "
           << stats.session_id << ",\n"
              "    \"run_succeeded\": "
           << (report.run_succeeded ? "true" : "false") << ",\n"
              "    \"final_state\": ";
    write_json_string(output, state_name(stats.state));
    output << ",\n"
              "    \"duration_ms\": "
           << milliseconds(report.session_duration) << ",\n"
              "    \"started_sessions\": "
           << stats.started_sessions << ",\n"
              "    \"completed_sessions\": "
           << stats.completed_sessions << ",\n"
              "    \"failed_sessions\": "
           << stats.failed_sessions << "\n"
              "  },\n"
              "  \"capture\": {\n"
              "    \"published_frames\": "
           << stats.capture.published_frames << ",\n"
              "    \"scheduler_skipped_ticks\": "
           << stats.capture.scheduler_skipped_ticks << ",\n"
              "    \"store_replaced_frames\": "
           << stats.capture.store_replaced_frames << ",\n"
              "    \"source_width\": "
           << stats.capture.source_width << ",\n"
              "    \"source_height\": "
           << stats.capture.source_height << "\n"
              "  },\n"
              "  \"encoder\": {\n"
              "    \"consumed_frames\": "
           << stats.encoder.consumed_frames << ",\n"
              "    \"submitted_access_units\": "
           << stats.encoder.submitted_access_units << ",\n"
              "    \"key_frames\": "
           << stats.encoder.key_frames << ",\n"
              "    \"encoded_bytes\": "
           << stats.encoder.encoded_bytes << "\n"
              "  },\n"
              "  \"rtp\": {\n"
              "    \"media_datagrams\": "
           << stats.output.emitted_units << ",\n"
              "    \"media_bytes\": "
           << stats.output.emitted_bytes << ",\n"
              "    \"input_access_units\": "
           << stats.output.consumed_access_units << ",\n"
              "    \"input_bytes\": "
           << stats.output.input_bytes << "\n"
              "  },\n";
    write_rtcp_stats(output, stats);
    output << "}\n";
    return std::move(output).str();
}

PublisherSessionReportWriteResult write_publisher_session_report_json(
    const std::filesystem::path& path,
    const PublisherSessionReport& report) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        return std::unexpected{"failed to open publisher stats output: " +
                               path_text(path)};
    }
    output << render_publisher_session_report_json(report);
    output.flush();
    if (!output) {
        return std::unexpected{"failed to write publisher stats output: " +
                               path_text(path)};
    }
    return {};
}

}  // namespace semilive::publisher::reporting
