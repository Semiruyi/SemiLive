#pragma once

#include <semilive/publisher/application/publisher_controller/publisher_controller.hpp>
#include <semilive/publisher/composition/publisher_config.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <string>

namespace semilive::publisher::reporting {

struct PublisherSessionReport {
    composition::PublisherConfig config;
    application::PublisherControllerStats stats;
    std::chrono::nanoseconds session_duration{};
    bool run_succeeded = false;
};

using PublisherSessionReportWriteResult = std::expected<void, std::string>;

[[nodiscard]] std::string render_publisher_session_report_json(
    const PublisherSessionReport& report);

[[nodiscard]] PublisherSessionReportWriteResult
write_publisher_session_report_json(
    const std::filesystem::path& path,
    const PublisherSessionReport& report);

}  // namespace semilive::publisher::reporting
