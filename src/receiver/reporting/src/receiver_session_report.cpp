#include <semilive/receiver/reporting/receiver_session_report.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace semilive::receiver::reporting {
namespace {

using Stats = application::ReceiverControllerStats;

void write_json_string(std::ostream& output, const std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output.put('"');
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
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

[[nodiscard]] std::string path_text(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

[[nodiscard]] std::int64_t milliseconds(
    const std::chrono::nanoseconds duration) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration)
        .count();
}

void write_optional_duration(
    std::ostream& output,
    const std::optional<std::chrono::nanoseconds>& duration) {
    if (!duration) {
        output << "null";
        return;
    }
    output << milliseconds(*duration);
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

[[nodiscard]] std::string_view state_name(
    const domain::VideoReceiveWorkerState state) noexcept {
    using State = domain::VideoReceiveWorkerState;
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

[[nodiscard]] std::string_view recovery_state_name(
    const domain::H264RecoveryState state) noexcept {
    return state == domain::H264RecoveryState::Streaming
               ? "streaming"
               : "waiting_for_random_access";
}

void write_config(std::ostream& output,
                  const composition::ReceiverConfig& config) {
    output << "  \"config\": {\n"
              "    \"input\": {\n"
              "      \"bind_address\": ";
    write_json_string(output, config.input.bind_address);
    output << ",\n"
              "      \"bind_port\": "
           << config.input.bind_port << ",\n"
              "      \"maximum_datagram_bytes\": "
           << config.input.maximum_datagram_bytes << ",\n"
              "      \"receive_buffer_bytes\": "
           << config.input.receive_buffer_bytes << "\n"
              "    },\n"
              "    \"rtp\": {\n"
              "      \"payload_type\": "
           << static_cast<unsigned int>(config.pipeline.session.payload_type)
           << ",\n"
              "      \"configured_ssrc\": ";
    write_optional_integer(output, config.pipeline.session.ssrc);
    output << ",\n"
              "      \"reorder_maximum_buffered_packets\": "
           << config.pipeline.reorder.maximum_buffered_packets << ",\n"
              "      \"reorder_maximum_hold_ms\": "
           << config.pipeline.reorder.maximum_hold_time.count() << ",\n"
              "      \"clock_rate\": "
           << config.pipeline.timestamp_mapper.clock_rate << "\n"
              "    },\n"
              "    \"h264\": {\n"
              "      \"maximum_nal_unit_bytes\": "
           << config.pipeline.depacketizer.maximum_nal_unit_bytes << ",\n"
              "      \"maximum_access_unit_bytes\": "
           << config.pipeline.assembler.maximum_access_unit_bytes << ",\n"
              "      \"maximum_nal_units_per_access_unit\": "
           << config.pipeline.assembler.maximum_nal_units << "\n"
              "    },\n"
              "    \"output_path\": ";
    write_json_string(output, path_text(config.h264_output_path));
    output << ",\n"
              "    \"receive_poll_interval_ms\": "
           << config.receive_poll_interval.count() << ",\n"
              "    \"output_stall_threshold_ms\": "
           << config.output_stall_threshold.count() << "\n"
              "  },\n";
}

void write_session(std::ostream& output,
                   const Stats& stats,
                   const bool run_succeeded) {
    output << "  \"session\": {\n"
              "    \"id\": "
           << stats.session_id << ",\n"
              "    \"run_succeeded\": "
           << (run_succeeded ? "true" : "false") << ",\n"
              "    \"final_state\": ";
    write_json_string(output, state_name(stats.state));
    output << ",\n"
              "    \"duration_ms\": "
           << milliseconds(stats.session_duration) << ",\n"
              "    \"first_output_delay_ms\": ";
    write_optional_duration(output, stats.first_output_delay);
    output << ",\n"
              "    \"maximum_output_gap_ms\": ";
    write_optional_duration(output, stats.maximum_output_gap);
    output << ",\n"
              "    \"terminal_output_gap_ms\": ";
    write_optional_duration(output, stats.terminal_output_gap);
    output << ",\n"
              "    \"output_stall_events\": "
           << stats.output_stall_events << ",\n"
              "    \"output_stall_excess_total_ms\": "
           << milliseconds(stats.output_stall_excess_total) << ",\n"
              "    \"started_sessions\": "
           << stats.started_sessions << ",\n"
              "    \"completed_sessions\": "
           << stats.completed_sessions << ",\n"
              "    \"failed_sessions\": "
           << stats.failed_sessions << ",\n"
              "    \"start_failures\": "
           << stats.start_failures << "\n"
              "  },\n";
}

void write_input(std::ostream& output, const Stats& stats) {
    output << "  \"input\": {\n"
              "    \"received_datagrams\": "
           << stats.received_datagrams << ",\n"
              "    \"received_bytes\": "
           << stats.received_bytes << ",\n"
              "    \"receive_timeouts\": "
           << stats.receive_timeouts << ",\n"
              "    \"bound_address\": ";
    if (stats.input) {
        write_json_string(output, stats.input->bound_address);
    } else {
        output << "null";
    }
    output << ",\n"
              "    \"bound_port\": ";
    if (stats.input) {
        output << stats.input->bound_port;
    } else {
        output << "null";
    }
    output << ",\n"
              "    \"actual_receive_buffer_bytes\": ";
    if (stats.input) {
        output << stats.input->receive_buffer_bytes;
    } else {
        output << "null";
    }
    output << "\n  },\n";
}

void write_rtp(std::ostream& output, const Stats& stats) {
    const auto& pipeline = stats.pipeline;
    const auto& session = pipeline.session_filter;
    const auto& reorder = pipeline.reorder;
    output << "  \"rtp\": {\n"
              "    \"received_datagrams\": "
           << pipeline.received_datagrams << ",\n"
              "    \"parse_failures\": "
           << pipeline.parse_failures << ",\n"
              "    \"session_drops\": "
           << pipeline.session_drops << ",\n"
              "    \"session_filter\": {\n"
              "      \"bound_ssrc\": ";
    write_optional_integer(output, session.bound_ssrc);
    output << ",\n"
              "      \"accepted_packets\": "
           << session.accepted_packets << ",\n"
              "      \"payload_type_mismatches\": "
           << session.payload_type_mismatches << ",\n"
              "      \"ssrc_mismatches\": "
           << session.ssrc_mismatches << "\n"
              "    },\n"
              "    \"reorder\": {\n"
              "      \"received_packets\": "
           << reorder.received_packets << ",\n"
              "      \"ordered_packets\": "
           << reorder.ordered_packets << ",\n"
              "      \"reordered_packets\": "
           << reorder.reordered_packets << ",\n"
              "      \"duplicate_packets\": "
           << reorder.duplicate_packets << ",\n"
              "      \"late_packets\": "
           << reorder.late_packets << ",\n"
              "      \"confirmed_gaps\": "
           << reorder.confirmed_gaps << ",\n"
              "      \"confirmed_lost_packets\": "
           << reorder.confirmed_lost_packets << ",\n"
              "      \"timeout_gaps\": "
           << reorder.timeout_gaps << ",\n"
              "      \"capacity_gaps\": "
           << reorder.capacity_gaps << ",\n"
              "      \"buffered_packets\": "
           << reorder.buffered_packets << ",\n"
              "      \"peak_buffered_packets\": "
           << reorder.peak_buffered_packets << "\n"
              "    }\n"
              "  },\n";
}

void write_h264(std::ostream& output, const Stats& stats) {
    const auto& depacketizer = stats.pipeline.depacketizer;
    const auto& assembler = stats.pipeline.assembler;
    const auto& recovery = stats.pipeline.recovery;
    output << "  \"h264\": {\n"
              "    \"depacketizer\": {\n"
              "      \"input_packets\": "
           << depacketizer.input_packets << ",\n"
              "      \"sequence_gaps\": "
           << depacketizer.sequence_gaps << ",\n"
              "      \"missing_rtp_packets\": "
           << depacketizer.missing_rtp_packets << ",\n"
              "      \"completed_nal_units\": "
           << depacketizer.completed_nal_units << ",\n"
              "      \"single_nal_units\": "
           << depacketizer.single_nal_units << ",\n"
              "      \"fu_a_nal_units\": "
           << depacketizer.fu_a_nal_units << ",\n"
              "      \"discontinuities\": "
           << depacketizer.discontinuities << ",\n"
              "      \"dropped_packets\": "
           << depacketizer.dropped_packets << ",\n"
              "      \"malformed_packets\": "
           << depacketizer.malformed_packets << ",\n"
              "      \"unsupported_packets\": "
           << depacketizer.unsupported_packets << ",\n"
              "      \"orphan_fragments\": "
           << depacketizer.orphan_fragments << ",\n"
              "      \"abandoned_fragmented_nal_units\": "
           << depacketizer.abandoned_fragmented_nal_units << ",\n"
              "      \"oversized_nal_units\": "
           << depacketizer.oversized_nal_units << ",\n"
              "      \"pending_fragment_bytes\": "
           << depacketizer.pending_fragment_bytes << "\n"
              "    },\n"
              "    \"access_unit_assembler\": {\n"
              "      \"input_nal_units\": "
           << assembler.input_nal_units << ",\n"
              "      \"upstream_discontinuities\": "
           << assembler.upstream_discontinuities << ",\n"
              "      \"completed_access_units\": "
           << assembler.completed_access_units << ",\n"
              "      \"discarded_access_units\": "
           << assembler.discarded_access_units << ",\n"
              "      \"discarded_nal_units\": "
           << assembler.discarded_nal_units << ",\n"
              "      \"timestamp_discontinuities\": "
           << assembler.timestamp_discontinuities << ",\n"
              "      \"sequence_discontinuities\": "
           << assembler.sequence_discontinuities << ",\n"
              "      \"oversized_access_units\": "
           << assembler.oversized_access_units << ",\n"
              "      \"excessive_nal_unit_counts\": "
           << assembler.excessive_nal_unit_counts << ",\n"
              "      \"pending_bytes\": "
           << assembler.pending_bytes << ",\n"
              "      \"pending_nal_units\": "
           << assembler.pending_nal_units << ",\n"
              "      \"peak_pending_bytes\": "
           << assembler.peak_pending_bytes << "\n"
              "    },\n"
              "    \"recovery\": {\n"
              "      \"state\": ";
    write_json_string(output, recovery_state_name(recovery.state));
    output << ",\n"
              "      \"input_access_units\": "
           << recovery.input_access_units << ",\n"
              "      \"assembler_discontinuities\": "
           << recovery.assembler_discontinuities << ",\n"
              "      \"external_discontinuities\": "
           << recovery.external_discontinuities << ",\n"
              "      \"dropped_while_waiting\": "
           << recovery.dropped_while_waiting << ",\n"
              "      \"delivered_access_units\": "
           << recovery.delivered_access_units << ",\n"
              "      \"recovery_points\": "
           << recovery.recovery_points << ",\n"
              "      \"episodes_started\": "
           << recovery.recovery_episodes_started << ",\n"
              "      \"episodes_completed\": "
           << recovery.recovery_episodes_completed << ",\n"
              "      \"wait_total_ms\": "
           << milliseconds(recovery.recovery_wait_total) << ",\n"
              "      \"wait_maximum_ms\": "
           << milliseconds(recovery.recovery_wait_maximum) << ",\n"
              "      \"active_wait_ms\": ";
    write_optional_duration(output, recovery.active_recovery_wait);
    output << ",\n"
              "      \"wait_total_including_active_ms\": "
           << milliseconds(recovery.recovery_wait_total_including_active)
           << ",\n"
              "      \"wait_maximum_including_active_ms\": "
           << milliseconds(recovery.recovery_wait_maximum_including_active)
           << "\n"
              "    }\n"
              "  },\n";
}

void write_output(std::ostream& output, const Stats& stats) {
    output << "  \"output\": {\n"
              "    \"name\": ";
    if (stats.output) {
        write_json_string(output, stats.output->output_name);
    } else {
        output << "null";
    }
    output << ",\n"
              "    \"pipeline_access_units\": "
           << stats.pipeline.output_access_units << ",\n"
              "    \"timestamp_mapping_failures\": "
           << stats.pipeline.timestamp_mapping_failures << ",\n"
              "    \"submitted_access_units\": "
           << stats.submitted_access_units << ",\n"
              "    \"submitted_bytes\": "
           << stats.submitted_bytes << ",\n"
              "    \"backpressure_drops\": "
           << stats.backpressure_drops << ",\n"
              "    \"discarded_after_backpressure\": "
           << stats.discarded_after_backpressure << "\n"
              "  }\n";
}

}  // namespace

std::string render_receiver_session_report_json(
    const ReceiverSessionReport& report) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\n"
              "  \"schema_version\": 1,\n"
              "  \"application\": \"semilive_receiver\",\n"
              "  \"application_version\": \"0.1.0-dev\",\n";
    write_config(output, report.config);
    write_session(output, report.stats, report.run_succeeded);
    write_input(output, report.stats);
    write_rtp(output, report.stats);
    write_h264(output, report.stats);
    write_output(output, report.stats);
    output << "}\n";
    return std::move(output).str();
}

ReceiverSessionReportWriteResult write_receiver_session_report_json(
    const std::filesystem::path& path,
    const ReceiverSessionReport& report) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        return std::unexpected{"failed to open receiver stats output: " +
                               path_text(path)};
    }

    output << render_receiver_session_report_json(report);
    output.flush();
    if (!output) {
        return std::unexpected{"failed to write receiver stats output: " +
                               path_text(path)};
    }
    return {};
}

}  // namespace semilive::receiver::reporting
