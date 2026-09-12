#include <semilive/publisher/composition/publisher_composition.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace app = semilive::publisher::application;
namespace composition = semilive::publisher::composition;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

[[nodiscard]] composition::PublisherConfig valid_config() {
    composition::PublisherConfig config;
    config.output.rtp_udp.destination_address = "127.0.0.1";
    config.output.rtp_udp.destination_port = 5004;
    return config;
}

void invalid_rtp_output_is_rejected_before_assembly() {
    const auto check_invalid = [](composition::PublisherConfig config,
                                  const std::string_view message) {
        composition::PublisherComposition graph{std::move(config)};
        const auto result = graph.assemble();
        require(!result, message);
        require(result.error().operation ==
                    composition::PublisherCompositionOperation::ValidateConfig,
                "invalid RTP output must report config validation");
        require(graph.controller() == nullptr,
                "failed composition must not expose a controller");
    };

    check_invalid(composition::PublisherConfig{},
                  "empty RTP endpoint must fail composition assembly");

    auto zero_port = valid_config();
    zero_port.output.rtp_udp.destination_port = 0;
    check_invalid(std::move(zero_port),
                  "zero RTP port must fail composition assembly");

    auto static_payload_type = valid_config();
    static_payload_type.output.rtp_udp.payload_type = 95;
    check_invalid(std::move(static_payload_type),
                  "static RTP payload type must fail composition assembly");

    auto small_datagram = valid_config();
    small_datagram.output.rtp_udp.max_datagram_bytes = 14;
    check_invalid(std::move(small_datagram),
                  "undersized RTP datagram must fail composition assembly");
}

void invalid_recovery_timeout_is_rejected_before_assembly() {
    auto config = valid_config();
    config.video.recovery_timeout = std::chrono::milliseconds::zero();
    composition::PublisherComposition graph{std::move(config)};

    const auto result = graph.assemble();
    require(!result, "zero recovery timeout must fail composition assembly");
    require(result.error().operation ==
                composition::PublisherCompositionOperation::ValidateConfig,
            "zero recovery timeout must report config validation");
    require(graph.controller() == nullptr,
            "invalid composition must not expose a controller");
}

void dispose_before_assembly_is_idempotent_and_terminal() {
    composition::PublisherComposition graph{composition::PublisherConfig{}};

    require(graph.dispose().has_value(),
            "unassembled composition must dispose successfully");
    require(graph.dispose().has_value(),
            "composition disposal must be idempotent");
    require(graph.controller() == nullptr,
            "disposed composition must not expose a controller");

    const auto assembled = graph.assemble();
    require(!assembled, "disposed composition must not reassemble");
    require(assembled.error().operation ==
                composition::PublisherCompositionOperation::Control,
            "reassembly after disposal must report a control error");
}

#if defined(_WIN32)
void windows_composition_assembles_an_idle_video_graph() {
    composition::PublisherComposition graph{valid_config()};
    require(graph.controller() == nullptr,
            "unassembled composition must not expose a controller");

    const auto assembled = graph.assemble();
    require(assembled.has_value(),
            "valid Windows publisher composition must assemble");
    require(graph.controller() != nullptr,
            "assembled composition must expose its controller");
    require(graph.controller()->state() == app::PublisherControllerState::Idle,
            "assembled controller must start idle");

    const auto duplicate = graph.assemble();
    require(!duplicate, "composition must reject duplicate assembly");
    require(duplicate.error().operation ==
                composition::PublisherCompositionOperation::Control,
            "duplicate assembly must report a control error");

    require(graph.dispose().has_value(),
            "idle composition must dispose successfully");
    require(graph.controller() == nullptr,
            "disposed composition must invalidate controller access");
    require(graph.dispose().has_value(),
            "disposed composition must accept repeated disposal");
}

void const_composition_exposes_const_controller_access() {
    composition::PublisherComposition graph{valid_config()};
    require(graph.assemble().has_value(),
            "composition must assemble for const access test");

    const auto& const_graph = graph;
    require(const_graph.controller() != nullptr,
            "const composition must expose its assembled controller");
    require(graph.dispose().has_value(),
            "const access test composition must dispose successfully");
}
#else
void non_windows_composition_reports_platform_error() {
    composition::PublisherComposition graph{valid_config()};

    const auto result = graph.assemble();
    require(!result, "non-Windows production composition must not assemble");
    require(result.error().operation ==
                composition::PublisherCompositionOperation::CheckPlatform,
            "non-Windows assembly must report platform support");
    require(graph.controller() == nullptr,
            "unsupported composition must not expose a controller");
    require(graph.dispose().has_value(),
            "unsupported composition must remain safely disposable");
}
#endif

}  // namespace

int main() {
    try {
        invalid_rtp_output_is_rejected_before_assembly();
        invalid_recovery_timeout_is_rejected_before_assembly();
        dispose_before_assembly_is_idempotent_and_terminal();
#if defined(_WIN32)
        windows_composition_assembles_an_idle_video_graph();
        const_composition_exposes_const_controller_access();
#else
        non_windows_composition_reports_platform_error();
#endif
    } catch (const std::exception& error) {
        std::cerr << "publisher composition test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
