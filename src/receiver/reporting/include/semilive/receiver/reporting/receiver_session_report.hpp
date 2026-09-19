#pragma once

#include <semilive/receiver/application/receiver_controller.hpp>
#include <semilive/receiver/composition/receiver_config.hpp>

#include <expected>
#include <filesystem>
#include <string>

namespace semilive::receiver::reporting {

struct ReceiverSessionReport {
    composition::ReceiverConfig config;
    application::ReceiverControllerStats stats;
    bool run_succeeded = false;
};

using ReceiverSessionReportWriteResult = std::expected<void, std::string>;

[[nodiscard]] std::string render_receiver_session_report_json(
    const ReceiverSessionReport& report);

[[nodiscard]] ReceiverSessionReportWriteResult
write_receiver_session_report_json(
    const std::filesystem::path& path,
    const ReceiverSessionReport& report);

}  // namespace semilive::receiver::reporting
