#include <semilive/common/log/log.hpp>
#include <semilive/receiver/composition/receiver_composition.hpp>

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#define SEMILIVE_LOG_TAG "receiver_main"

namespace {

using namespace std::chrono_literals;

namespace app = semilive::receiver::application;
namespace composition = semilive::receiver::composition;
namespace domain = semilive::receiver::domain;

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void handle_stop_signal(int) {
    stop_requested = 1;
}

enum class CommandLineAction : std::uint8_t {
    Run,
    Help,
    Version,
};

struct CommandLineOptions {
    CommandLineAction action = CommandLineAction::Run;
    composition::ReceiverConfig receiver;
};

using CommandLineResult = std::expected<CommandLineOptions, std::string>;

class LogLifetime final {
public:
    ~LogLifetime() {
        semilive::log::shutdown();
    }

    LogLifetime(const LogLifetime&) = delete;
    LogLifetime& operator=(const LogLifetime&) = delete;

    LogLifetime() = default;
};

void print_help() {
    std::cout
        << "SemiLive H.264/RTP receiver\n"
           "Usage: semilive_receiver [options]\n\n"
           "Options:\n"
           "  --bind-address ADDRESS          Numeric IPv4 or IPv6 listen "
           "address\n"
           "                                  (default: 0.0.0.0)\n"
           "  --bind-port PORT                UDP listen port "
           "(default: 5004)\n"
           "  --rtp-payload-type PT           Dynamic RTP payload type\n"
           "                                  (default: 96)\n"
           "  --rtp-ssrc SSRC                 Accept only this decimal SSRC\n"
           "                                  (default: bind first source)\n"
           "  --rtp-max-datagram-bytes SIZE   Maximum accepted UDP payload\n"
           "                                  (default: 65507)\n"
           "  --udp-receive-buffer-bytes SIZE Requested kernel UDP receive "
           "buffer\n"
           "                                  (default: 4194304)\n"
           "  --output PATH                   Annex-B H.264 output file\n"
           "                                  (default: semilive-received.h264;\n"
           "                                   existing file is replaced)\n"
           "  --poll-interval-ms MS           Receive/timer poll interval\n"
           "                                  (default: 10, range: 1..1000)\n"
           "  --help                          Show this help\n"
           "  --version                       Show the version\n";
}

[[nodiscard]] std::optional<std::uint64_t> parse_unsigned(
    const std::string_view text) {
    std::uint64_t value = 0;
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

CommandLineResult parse_command_line(const int argc, char* argv[]) {
    CommandLineOptions options;
    bool bind_address_set = false;
    bool bind_port_set = false;
    bool payload_type_set = false;
    bool ssrc_set = false;
    bool maximum_datagram_set = false;
    bool receive_buffer_set = false;
    bool output_set = false;
    bool poll_interval_set = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help" || argument == "--version") {
            if (argc != 2) {
                return std::unexpected{
                    std::string{argument} +
                    " cannot be combined with other arguments"};
            }
            options.action = argument == "--help"
                                 ? CommandLineAction::Help
                                 : CommandLineAction::Version;
            return options;
        }

        if (argument == "--bind-address") {
            if (bind_address_set) {
                return std::unexpected{
                    "--bind-address may only be specified once"};
            }
            if (++index >= argc || std::string_view{argv[index]}.empty()) {
                return std::unexpected{
                    "--bind-address requires an address"};
            }
            options.receiver.input.bind_address = argv[index];
            bind_address_set = true;
            continue;
        }

        if (argument == "--bind-port") {
            if (bind_port_set) {
                return std::unexpected{
                    "--bind-port may only be specified once"};
            }
            if (++index >= argc) {
                return std::unexpected{"--bind-port requires a value"};
            }
            const auto value = parse_unsigned(argv[index]);
            if (!value || *value == 0 || *value > 65'535) {
                return std::unexpected{
                    "--bind-port requires an integer in 1..65535"};
            }
            options.receiver.input.bind_port =
                static_cast<std::uint16_t>(*value);
            bind_port_set = true;
            continue;
        }

        if (argument == "--rtp-payload-type") {
            if (payload_type_set) {
                return std::unexpected{
                    "--rtp-payload-type may only be specified once"};
            }
            if (++index >= argc) {
                return std::unexpected{
                    "--rtp-payload-type requires a value"};
            }
            const auto value = parse_unsigned(argv[index]);
            if (!value || *value < 96 || *value > 127) {
                return std::unexpected{
                    "--rtp-payload-type requires an integer in 96..127"};
            }
            options.receiver.pipeline.session.payload_type =
                static_cast<std::uint8_t>(*value);
            payload_type_set = true;
            continue;
        }

        if (argument == "--rtp-ssrc") {
            if (ssrc_set) {
                return std::unexpected{
                    "--rtp-ssrc may only be specified once"};
            }
            if (++index >= argc) {
                return std::unexpected{"--rtp-ssrc requires a value"};
            }
            const auto value = parse_unsigned(argv[index]);
            if (!value ||
                *value > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected{
                    "--rtp-ssrc requires an integer in 0..4294967295"};
            }
            options.receiver.pipeline.session.ssrc =
                static_cast<std::uint32_t>(*value);
            ssrc_set = true;
            continue;
        }

        if (argument == "--rtp-max-datagram-bytes") {
            if (maximum_datagram_set) {
                return std::unexpected{
                    "--rtp-max-datagram-bytes may only be specified once"};
            }
            if (++index >= argc) {
                return std::unexpected{
                    "--rtp-max-datagram-bytes requires a value"};
            }
            const auto value = parse_unsigned(argv[index]);
            if (!value || *value < 15 || *value > 65'507) {
                return std::unexpected{
                    "--rtp-max-datagram-bytes requires an integer in "
                    "15..65507"};
            }
            options.receiver.input.maximum_datagram_bytes =
                static_cast<std::size_t>(*value);
            maximum_datagram_set = true;
            continue;
        }

        if (argument == "--udp-receive-buffer-bytes") {
            if (receive_buffer_set) {
                return std::unexpected{
                    "--udp-receive-buffer-bytes may only be specified "
                    "once"};
            }
            if (++index >= argc) {
                return std::unexpected{
                    "--udp-receive-buffer-bytes requires a value"};
            }
            const auto value = parse_unsigned(argv[index]);
            if (!value || *value == 0 ||
                *value >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<int>::max())) {
                return std::unexpected{
                    "--udp-receive-buffer-bytes requires an integer in "
                    "1..2147483647"};
            }
            options.receiver.input.receive_buffer_bytes =
                static_cast<std::size_t>(*value);
            receive_buffer_set = true;
            continue;
        }

        if (argument == "--output") {
            if (output_set) {
                return std::unexpected{
                    "--output may only be specified once"};
            }
            if (++index >= argc || std::string_view{argv[index]}.empty()) {
                return std::unexpected{"--output requires a path"};
            }
            options.receiver.h264_output_path =
                std::filesystem::path{argv[index]};
            output_set = true;
            continue;
        }

        if (argument == "--poll-interval-ms") {
            if (poll_interval_set) {
                return std::unexpected{
                    "--poll-interval-ms may only be specified once"};
            }
            if (++index >= argc) {
                return std::unexpected{
                    "--poll-interval-ms requires a value"};
            }
            const auto value = parse_unsigned(argv[index]);
            if (!value || *value == 0 || *value > 1'000) {
                return std::unexpected{
                    "--poll-interval-ms requires an integer in 1..1000"};
            }
            options.receiver.receive_poll_interval =
                std::chrono::milliseconds{
                    static_cast<std::chrono::milliseconds::rep>(*value)};
            poll_interval_set = true;
            continue;
        }

        return std::unexpected{"unknown argument: " +
                               std::string{argument}};
    }

    return options;
}

[[nodiscard]] std::string_view composition_operation_name(
    const composition::ReceiverCompositionOperation operation) {
    using Operation = composition::ReceiverCompositionOperation;
    switch (operation) {
    case Operation::Control:
        return "control";
    case Operation::CreateWorker:
        return "create-worker";
    case Operation::CreateController:
        return "create-controller";
    case Operation::StopSession:
        return "stop-session";
    case Operation::DisposeGraph:
        return "dispose-graph";
    }
    return "unknown";
}

[[nodiscard]] std::string_view controller_operation_name(
    const domain::VideoReceiveWorkerOperation operation) {
    using Operation = domain::VideoReceiveWorkerOperation;
    switch (operation) {
    case Operation::Control:
        return "control";
    case Operation::StartSession:
        return "start-session";
    case Operation::OpenOutput:
        return "open-output";
    case Operation::OpenInput:
        return "open-input";
    case Operation::ReceiveInput:
        return "receive-input";
    case Operation::SubmitOutput:
        return "submit-output";
    case Operation::RunSession:
        return "run-session";
    case Operation::StopSession:
        return "stop-session";
    case Operation::Internal:
        return "internal";
    }
    return "unknown";
}

void report_composition_issue(
    const std::string_view context,
    const composition::ReceiverCompositionIssue& issue) {
    const auto operation = composition_operation_name(issue.operation);
    std::cerr << context << " failed at " << operation << ": "
              << issue.message << '\n';
    SEMILIVE_LOG_ERROR("{} failed at {}: {}", context, operation,
                       issue.message);
}

void report_controller_issue(const std::string_view context,
                             const app::ReceiverControllerIssue& issue) {
    const auto operation = controller_operation_name(issue.operation);
    std::cerr << context << " failed at " << operation << ": "
              << issue.message << '\n';
    SEMILIVE_LOG_ERROR("{} failed at {}: {}", context, operation,
                       issue.message);
}

void print_started(const app::ReceiverStarted& started,
                   const app::ReceiverControllerStats& stats) {
    std::cout << "Receiving session " << started.session_id << '\n';
    if (stats.input) {
        std::cout << "  input: " << stats.input->bound_address << ':'
                  << stats.input->bound_port << " (UDP receive buffer: "
                  << stats.input->receive_buffer_bytes << " bytes)\n";
    }
    if (stats.output) {
        std::cout << "  output: " << stats.output->output_name << '\n';
    }
    std::cout << "Press Ctrl+C to stop.\n";

    SEMILIVE_LOG_INFO(
        "receiver session {} started: input={}:{}, "
        "udp_receive_buffer_bytes={}, output={}",
        started.session_id,
        stats.input ? stats.input->bound_address : std::string{"unknown"},
        stats.input ? stats.input->bound_port : 0,
        stats.input ? stats.input->receive_buffer_bytes : 0,
        stats.output ? stats.output->output_name : std::string{"unknown"});
}

void print_final_stats(const app::ReceiverControllerStats& stats) {
    const auto& pipeline = stats.pipeline;
    std::cout << "Receiver statistics\n"
              << "  session: " << stats.session_id << '\n'
              << "  input: " << stats.received_datagrams
              << " datagrams, " << stats.receive_timeouts << " timeouts, "
              << pipeline.parse_failures << " malformed, "
              << pipeline.session_drops << " session drops\n"
              << "  reorder: " << pipeline.reorder.reordered_packets
              << " reordered, " << pipeline.reorder.confirmed_gaps
              << " gaps, " << pipeline.reorder.confirmed_lost_packets
              << " packets lost\n"
              << "  recovery: " << pipeline.recovery.recovery_points
              << " recovery points, "
              << pipeline.recovery.dropped_while_waiting
              << " AUs dropped while waiting\n"
              << "  output: " << stats.submitted_access_units << " AUs, "
              << stats.submitted_bytes << " bytes, "
              << stats.backpressure_drops << " backpressure drops\n";

    SEMILIVE_LOG_INFO(
        "receiver session {} final stats: datagrams={}, timeouts={}, "
        "parse_failures={}, session_drops={}, reordered={}, gaps={}, "
        "lost_packets={}, recovery_points={}, recovery_drops={}, "
        "output_units={}, output_bytes={}, backpressure_drops={}",
        stats.session_id, stats.received_datagrams, stats.receive_timeouts,
        pipeline.parse_failures, pipeline.session_drops,
        pipeline.reorder.reordered_packets,
        pipeline.reorder.confirmed_gaps,
        pipeline.reorder.confirmed_lost_packets,
        pipeline.recovery.recovery_points,
        pipeline.recovery.dropped_while_waiting,
        stats.submitted_access_units, stats.submitted_bytes,
        stats.backpressure_drops);
}

[[nodiscard]] bool dispose_composition(
    composition::ReceiverComposition& graph) {
    const auto disposed = graph.dispose();
    if (disposed) {
        return true;
    }
    report_composition_issue("receiver disposal", disposed.error());
    return false;
}

int run_receiver(composition::ReceiverConfig config) {
    composition::ReceiverComposition graph{std::move(config)};
    const auto assembled = graph.assemble();
    if (!assembled) {
        report_composition_issue("receiver assembly", assembled.error());
        return EXIT_FAILURE;
    }

    auto* controller = graph.controller();
    if (controller == nullptr) {
        std::cerr << "Receiver assembly returned no controller.\n";
        SEMILIVE_LOG_ERROR("receiver assembly returned no controller");
        (void)dispose_composition(graph);
        return EXIT_FAILURE;
    }

    const auto started = controller->start_receiving();
    if (!started) {
        report_controller_issue("receiver start", started.error());
        (void)dispose_composition(graph);
        return EXIT_FAILURE;
    }

    print_started(*started, controller->stats());
    bool succeeded = true;
    while (stop_requested == 0) {
        const auto terminal = controller->wait_for_terminal_for(250ms);
        if (terminal.status == app::ReceiverWaitStatus::Timeout) {
            continue;
        }
        if (terminal.status == app::ReceiverWaitStatus::Failed) {
            if (terminal.issue) {
                report_controller_issue("receiver session", *terminal.issue);
            } else {
                std::cerr << "Receiver session failed without an issue.\n";
                SEMILIVE_LOG_ERROR(
                    "receiver session failed without an issue");
            }
            succeeded = false;
            break;
        }

        std::cerr << "Receiver session stopped unexpectedly.\n";
        SEMILIVE_LOG_ERROR("receiver session stopped unexpectedly");
        succeeded = false;
        break;
    }

    if (stop_requested != 0) {
        std::cout << "Stopping receiver...\n";
        SEMILIVE_LOG_INFO("receiver stop requested by signal");
    }

    const auto stopped = controller->stop_receiving();
    if (!stopped) {
        report_controller_issue("receiver stop", stopped.error());
        succeeded = false;
    }

    print_final_stats(controller->stats());
    if (!dispose_composition(graph)) {
        succeeded = false;
    }
    return succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace

int main(int argc, char* argv[]) {
    const auto options = parse_command_line(argc, argv);
    if (!options) {
        std::cerr << options.error() << ". Use --help for usage.\n";
        return EXIT_FAILURE;
    }
    if (options->action == CommandLineAction::Help) {
        print_help();
        return EXIT_SUCCESS;
    }
    if (options->action == CommandLineAction::Version) {
        std::cout << "semilive_receiver 0.1.0-dev\n";
        return EXIT_SUCCESS;
    }

    semilive::log::Config log_config;
    log_config.file_path = "logs/semilive_receiver.log";
    const auto log_result = semilive::log::init(log_config);
    if (log_result == semilive::log::InitResult::Failed) {
        std::cerr << "Failed to initialize logging.\n";
        return EXIT_FAILURE;
    }
    const LogLifetime log_lifetime;

    SEMILIVE_LOG_INFO("receiver process started");
    if (std::signal(SIGINT, handle_stop_signal) == SIG_ERR ||
        std::signal(SIGTERM, handle_stop_signal) == SIG_ERR) {
        std::cerr << "Failed to install stop signal handlers.\n";
        SEMILIVE_LOG_ERROR("failed to install stop signal handlers");
        return EXIT_FAILURE;
    }

    try {
        const auto result = run_receiver(options->receiver);
        SEMILIVE_LOG_INFO("receiver process stopped with exit code {}",
                          result);
        return result;
    } catch (const std::exception& error) {
        std::cerr << "Receiver terminated with an exception: "
                  << error.what() << '\n';
        SEMILIVE_LOG_CRITICAL("receiver terminated with an exception: {}",
                              error.what());
    } catch (...) {
        std::cerr << "Receiver terminated with an unknown exception.\n";
        SEMILIVE_LOG_CRITICAL(
            "receiver terminated with an unknown exception");
    }

    return EXIT_FAILURE;
}
