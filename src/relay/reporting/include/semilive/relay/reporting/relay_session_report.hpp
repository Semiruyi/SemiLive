#pragma once

#include <semilive/relay/application/relay_session.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <string>

namespace semilive::relay::reporting {

struct RelaySessionReport {
    application::RelayConfig config;
    application::RelaySessionInfo info;
    application::RelaySessionStats stats;
    std::chrono::nanoseconds session_duration{};
    bool run_succeeded = false;
};

using RelaySessionReportWriteResult = std::expected<void, std::string>;

[[nodiscard]] std::string render_relay_session_report_json(
    const RelaySessionReport& report);

[[nodiscard]] RelaySessionReportWriteResult
write_relay_session_report_json(
    const std::filesystem::path& path,
    const RelaySessionReport& report);

}  // namespace semilive::relay::reporting
