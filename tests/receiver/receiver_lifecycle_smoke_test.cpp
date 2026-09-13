#include <semilive/receiver/composition/receiver_composition.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using namespace std::chrono_literals;

namespace app = semilive::receiver::application;
namespace composition = semilive::receiver::composition;
namespace domain = semilive::receiver::domain;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

class TemporaryOutputFile final {
public:
    TemporaryOutputFile() {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::current_path() /
                ("semilive_receiver_lifecycle_" + std::to_string(nonce) +
                 ".h264");
    }

    ~TemporaryOutputFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void graph_runs_two_bounded_lifecycle_sessions() {
    TemporaryOutputFile output_file;
    composition::ReceiverConfig config;
    config.input.bind_address = "127.0.0.1";
    config.input.bind_port = 0;
    config.h264_output_path = output_file.path();
    config.receive_poll_interval = 1ms;
    composition::ReceiverComposition graph{std::move(config)};
    require(graph.controller() == nullptr,
            "unassembled graph must not expose a controller");
    require(graph.assemble().has_value(),
            "receiver composition must assemble");

    auto* controller = graph.controller();
    require(controller != nullptr,
            "assembled graph must expose a controller");
    require(controller->state() == app::ReceiverControllerState::Idle,
            "assembled receiver must start idle");

    const auto first = controller->start_receiving();
    require(first.has_value(), "first receive session must start");
    require(first->session_id == 1,
            "first receive session must use id 1");
    require(controller->state() == app::ReceiverControllerState::Running,
            "started receiver must be running");
    const auto running_stats = controller->stats();
    require(running_stats.input && running_stats.input->bound_port != 0,
            "running graph must own a bound UDP endpoint");
    require(running_stats.output &&
                running_stats.output->output_name ==
                    output_file.path().string(),
            "running graph must own the configured file output");

    const auto terminal = controller->wait_for_terminal_for(5ms);
    require(terminal.status == app::ReceiverWaitStatus::Timeout,
            "running skeleton must not terminate by itself");

    const auto duplicate = controller->start_receiving();
    require(!duplicate, "running receiver must reject duplicate start");
    require(duplicate.error().operation ==
                domain::VideoReceiveWorkerOperation::Control,
            "duplicate start must report a control issue");

    const auto stopped = controller->stop_receiving();
    require(stopped.has_value(), "first receive session must stop");
    require(stopped->session_id == first->session_id,
            "stop must report the active session id");
    require(controller->state() == app::ReceiverControllerState::Idle,
            "stopped receiver must return to idle");

    require(controller->stop_receiving().has_value(),
            "stopping an idle receiver must be idempotent");

    const auto second = controller->start_receiving();
    require(second.has_value(), "second receive session must start");
    require(second->session_id == 2,
            "restarted receiver must use a new session id");

    require(graph.dispose().has_value(),
            "disposing an active graph must stop its session");
    require(graph.controller() == nullptr,
            "disposed graph must invalidate controller access");
    require(graph.dispose().has_value(),
            "composition disposal must be idempotent");
}

void composition_rejects_duplicate_assembly() {
    composition::ReceiverComposition graph;
    require(graph.assemble().has_value(),
            "receiver composition must assemble once");

    const auto duplicate = graph.assemble();
    require(!duplicate, "receiver composition must reject reassembly");
    require(duplicate.error().operation ==
                composition::ReceiverCompositionOperation::Control,
            "reassembly must report a control issue");
    require(graph.dispose().has_value(),
            "duplicate assembly test graph must dispose");
}

}  // namespace

int main() {
    try {
        graph_runs_two_bounded_lifecycle_sessions();
        composition_rejects_duplicate_assembly();
    } catch (const std::exception& error) {
        std::cerr << "receiver lifecycle smoke test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
