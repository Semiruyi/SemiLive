#include <semilive/common/log/log.hpp>
#include <semilive/publisher/composition/publisher_composition.hpp>

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#define SEMILIVE_LOG_TAG "publisher_main"

namespace {

using namespace std::chrono_literals;

namespace app = semilive::publisher::application;
namespace composition = semilive::publisher::composition;
namespace capture = semilive::publisher::contracts::capture;

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void handle_stop_signal(int) {
    stop_requested = 1;
}

enum class CommandLineAction {
    Run,
    Help,
    Version,
};

struct CommandLineOptions {
    CommandLineAction action = CommandLineAction::Run;
    composition::PublisherConfig publisher;
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
        << "SemiLive desktop video publisher\n"
           "Usage: semilive_publisher [options]\n\n"
           "Options:\n"
           "  --rtp-address ADDRESS             RTP destination IPv4 or IPv6\n"
           "                                    address (required)\n"
           "  --rtp-port PORT                   RTP destination UDP port\n"
           "                                    (required, 1..65535)\n"
           "  --rtp-payload-type PT             Dynamic RTP payload type\n"
           "                                    (default: 96)\n"
           "  --rtp-max-datagram-bytes SIZE     Maximum UDP payload bytes\n"
           "                                    (default: 1200)\n"
           "  --display TARGET                  Capture primary or zero-based\n"
           "                                    display INDEX (default: primary)\n"
           "  --no-pointer                      Do not compose the mouse pointer\n"
           "  --help                            Show this help\n"
           "  --version                         Show the version\n";
}

CommandLineResult parse_command_line(const int argc, char* argv[]) {
    CommandLineOptions options;
    bool rtp_address_set = false;
    bool rtp_port_set = false;
    bool rtp_payload_type_set = false;
    bool rtp_max_datagram_bytes_set = false;
    bool display_set = false;
    bool pointer_disabled = false;

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

        if (argument == "--rtp-address") {
            if (rtp_address_set) {
                return std::unexpected{
                    "--rtp-address may only be specified once"};
            }
            if (++index >= argc) {
                return std::unexpected{"--rtp-address requires an address"};
            }
            const std::string_view address{argv[index]};
            if (address.empty()) {
                return std::unexpected{"--rtp-address requires an address"};
            }
            options.publisher.output.rtp_udp.destination_address = address;
            rtp_address_set = true;
            continue;
        }

        if (argument == "--rtp-port") {
            if (rtp_port_set) {
                return std::unexpected{
                    "--rtp-port may only be specified once"};
            }
            if (++index >= argc) {
                return std::unexpected{"--rtp-port requires a value"};
            }
            const std::string_view text{argv[index]};
            std::uint32_t port = 0;
            const auto parsed =
                std::from_chars(text.data(), text.data() + text.size(), port);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != text.data() + text.size() || port == 0 ||
                port > 65'535) {
                return std::unexpected{
                    "--rtp-port requires an integer in 1..65535"};
            }
            options.publisher.output.rtp_udp.destination_port =
                static_cast<std::uint16_t>(port);
            rtp_port_set = true;
            continue;
        }

        if (argument == "--rtp-payload-type") {
            if (rtp_payload_type_set) {
                return std::unexpected{
                    "--rtp-payload-type may only be specified once"};
            }
            if (++index >= argc) {
                return std::unexpected{
                    "--rtp-payload-type requires a value"};
            }
            const std::string_view text{argv[index]};
            std::uint32_t payload_type = 0;
            const auto parsed = std::from_chars(
                text.data(), text.data() + text.size(), payload_type);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != text.data() + text.size() ||
                payload_type < 96 || payload_type > 127) {
                return std::unexpected{
                    "--rtp-payload-type requires an integer in 96..127"};
            }
            options.publisher.output.rtp_udp.payload_type =
                static_cast<std::uint8_t>(payload_type);
            rtp_payload_type_set = true;
            continue;
        }

        if (argument == "--rtp-max-datagram-bytes") {
            if (rtp_max_datagram_bytes_set) {
                return std::unexpected{
                    "--rtp-max-datagram-bytes may only be specified once"};
            }
            if (++index >= argc) {
                return std::unexpected{
                    "--rtp-max-datagram-bytes requires a value"};
            }
            const std::string_view text{argv[index]};
            std::uint32_t max_datagram_bytes = 0;
            const auto parsed = std::from_chars(
                text.data(), text.data() + text.size(), max_datagram_bytes);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != text.data() + text.size() ||
                max_datagram_bytes < 15 || max_datagram_bytes > 65'507) {
                return std::unexpected{
                    "--rtp-max-datagram-bytes requires an integer in 15..65507"};
            }
            options.publisher.output.rtp_udp.max_datagram_bytes =
                max_datagram_bytes;
            rtp_max_datagram_bytes_set = true;
            continue;
        }

        if (argument == "--display") {
            if (display_set) {
                return std::unexpected{"--display may only be specified once"};
            }
            if (++index >= argc) {
                return std::unexpected{
                    "--display requires primary or a zero-based index"};
            }

            const std::string_view target{argv[index]};
            if (target == "primary") {
                options.publisher.video.capture.output = {
                    capture::DesktopOutputSelection::Primary, 0};
            } else {
                std::uint32_t display_index = 0;
                const auto* first = target.data();
                const auto* last = first + target.size();
                const auto parsed =
                    std::from_chars(first, last, display_index);
                if (parsed.ec != std::errc{} || parsed.ptr != last) {
                    return std::unexpected{
                        "--display requires primary or a zero-based index"};
                }
                options.publisher.video.capture.output = {
                    capture::DesktopOutputSelection::Index, display_index};
            }
            display_set = true;
            continue;
        }

        if (argument == "--no-pointer") {
            if (pointer_disabled) {
                return std::unexpected{
                    "--no-pointer may only be specified once"};
            }
            options.publisher.video.capture.compose_pointer = false;
            pointer_disabled = true;
            continue;
        }

        return std::unexpected{"unknown argument: " +
                               std::string{argument}};
    }

    if (!rtp_address_set && !rtp_port_set) {
        return std::unexpected{
            "--rtp-address and --rtp-port are required"};
    }
    if (!rtp_address_set) {
        return std::unexpected{
            "--rtp-address is required when --rtp-port is specified"};
    }
    if (!rtp_port_set) {
        return std::unexpected{
            "--rtp-port is required when --rtp-address is specified"};
    }
    return options;
}

std::string_view composition_operation_name(
    const composition::PublisherCompositionOperation operation) {
    using Operation = composition::PublisherCompositionOperation;
    switch (operation) {
    case Operation::Control:
        return "control";
    case Operation::ValidateConfig:
        return "validate-config";
    case Operation::CheckPlatform:
        return "check-platform";
    case Operation::CreateNotifier:
        return "create-notifier";
    case Operation::CreateResources:
        return "create-resources";
    case Operation::CreateOutputWorker:
        return "create-output-worker";
    case Operation::CreateEncoderWorker:
        return "create-encoder-worker";
    case Operation::CreateCaptureWorker:
        return "create-capture-worker";
    case Operation::CreateController:
        return "create-controller";
    case Operation::StopSession:
        return "stop-session";
    case Operation::DisposeGraph:
        return "dispose-graph";
    }
    return "unknown";
}

std::string_view controller_operation_name(
    const app::PublisherControllerOperation operation) {
    using Operation = app::PublisherControllerOperation;
    switch (operation) {
    case Operation::Control:
        return "control";
    case Operation::StartOutput:
        return "start-output";
    case Operation::StartEncoder:
        return "start-encoder";
    case Operation::StartCapture:
        return "start-capture";
    case Operation::VideoCaptureFailed:
        return "video-capture-failed";
    case Operation::VideoEncoderFailed:
        return "video-encoder-failed";
    case Operation::VideoOutputFailed:
        return "video-output-failed";
    case Operation::StopCapture:
        return "stop-capture";
    case Operation::DrainEncoder:
        return "drain-encoder";
    case Operation::DrainOutput:
        return "drain-output";
    case Operation::AbortPipeline:
        return "abort-pipeline";
    case Operation::ClearResources:
        return "clear-resources";
    case Operation::Internal:
        return "internal";
    }
    return "unknown";
}

void report_composition_issue(
    const std::string_view context,
    const composition::PublisherCompositionIssue& issue) {
    const auto operation = composition_operation_name(issue.operation);
    std::cerr << context << " failed at " << operation << ": "
              << issue.message << '\n';
    SEMILIVE_LOG_ERROR("{} failed at {}: {}", context, operation,
                       issue.message);
}

void report_controller_issue(const std::string_view context,
                             const app::PublisherControllerIssue& issue) {
    const auto operation = controller_operation_name(issue.operation);
    std::cerr << context << " failed at " << operation << ": "
              << issue.message << '\n';
    SEMILIVE_LOG_ERROR("{} failed at {}: {}", context, operation,
                       issue.message);
}

void print_started(const app::PublisherStarted& started) {
    const auto& source = started.capture.source;
    const auto& encoder = started.encoder.encoder;
    const auto& frame_rate = encoder.frame_rate;

    std::cout << "Publishing session " << started.session_id << '\n'
              << "  source: " << source.output_name << " ("
              << source.width << 'x' << source.height << ")\n"
              << "  encoder: " << encoder.encoder_name << " ("
              << encoder.output.width << 'x' << encoder.output.height << ", "
              << frame_rate.numerator << '/' << frame_rate.denominator
              << " fps, " << encoder.target_bit_rate << " bit/s)\n"
              << "  output: " << started.output.output.output_name << '\n'
              << "Press Ctrl+C to stop.\n";

    SEMILIVE_LOG_INFO(
        "publisher session {} started: source={} {}x{}, encoder={} {}x{} "
        "fps={}/{}, bitrate={}, output={}",
        started.session_id, source.output_name, source.width, source.height,
        encoder.encoder_name, encoder.output.width, encoder.output.height,
        frame_rate.numerator, frame_rate.denominator,
        encoder.target_bit_rate, started.output.output.output_name);
}

void print_final_stats(const app::PublisherControllerStats& stats) {
    std::cout << "Publisher statistics\n"
              << "  session: " << stats.session_id << '\n'
              << "  capture: " << stats.capture.published_frames
              << " frames published, "
              << stats.capture.scheduler_skipped_ticks
              << " scheduler ticks skipped, "
              << stats.capture.store_replaced_frames
              << " store frames replaced\n"
              << "  encoder: " << stats.encoder.submitted_access_units
              << " access units, " << stats.encoder.key_frames
              << " key frames, " << stats.encoder.encoded_bytes
              << " encoded bytes\n"
              << "  output: " << stats.output.emitted_units
              << " units, " << stats.output.emitted_bytes << " bytes\n";

    SEMILIVE_LOG_INFO(
        "publisher session {} final stats: capture_frames={}, "
        "scheduler_skipped={}, store_replaced={}, encoded_units={}, "
        "key_frames={}, encoded_bytes={}, output_units={}, output_bytes={}",
        stats.session_id, stats.capture.published_frames,
        stats.capture.scheduler_skipped_ticks,
        stats.capture.store_replaced_frames,
        stats.encoder.submitted_access_units, stats.encoder.key_frames,
        stats.encoder.encoded_bytes, stats.output.emitted_units,
        stats.output.emitted_bytes);
}

bool dispose_composition(composition::PublisherComposition& graph) {
    const auto disposed = graph.dispose();
    if (disposed) {
        return true;
    }
    report_composition_issue("publisher disposal", disposed.error());
    return false;
}

int run_publisher(composition::PublisherConfig config) {
    composition::PublisherComposition graph{std::move(config)};
    const auto assembled = graph.assemble();
    if (!assembled) {
        report_composition_issue("publisher assembly", assembled.error());
        return EXIT_FAILURE;
    }

    auto* controller = graph.controller();
    if (controller == nullptr) {
        std::cerr << "Publisher assembly returned no controller.\n";
        SEMILIVE_LOG_ERROR("publisher assembly returned no controller");
        (void)dispose_composition(graph);
        return EXIT_FAILURE;
    }

    const auto started = controller->start_publishing();
    if (!started) {
        report_controller_issue("publisher start", started.error());
        (void)dispose_composition(graph);
        return EXIT_FAILURE;
    }

    print_started(*started);
    bool succeeded = true;
    while (stop_requested == 0) {
        const auto terminal = controller->wait_for_terminal_for(250ms);
        if (terminal.status == app::PublisherWaitStatus::Timeout) {
            continue;
        }
        if (terminal.status == app::PublisherWaitStatus::Failed) {
            if (terminal.issue) {
                report_controller_issue("publisher session",
                                        *terminal.issue);
            } else {
                std::cerr << "Publisher session failed without an issue.\n";
                SEMILIVE_LOG_ERROR(
                    "publisher session failed without an issue");
            }
            succeeded = false;
            break;
        }

        std::cerr << "Publisher session stopped unexpectedly.\n";
        SEMILIVE_LOG_ERROR("publisher session stopped unexpectedly");
        succeeded = false;
        break;
    }

    if (stop_requested != 0) {
        std::cout << "Stopping publisher...\n";
        SEMILIVE_LOG_INFO("publisher stop requested by signal");
    }

    const auto stopped = controller->stop_publishing();
    if (!stopped) {
        report_controller_issue("publisher stop", stopped.error());
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
        std::cout << "semilive_publisher 0.1.0-dev\n";
        return EXIT_SUCCESS;
    }

    semilive::log::Config log_config;
    log_config.file_path = "logs/semilive_publisher.log";
    const auto log_result = semilive::log::init(log_config);
    if (log_result == semilive::log::InitResult::Failed) {
        std::cerr << "Failed to initialize logging.\n";
        return EXIT_FAILURE;
    }
    const LogLifetime log_lifetime;

    SEMILIVE_LOG_INFO("publisher process started");
    if (std::signal(SIGINT, handle_stop_signal) == SIG_ERR) {
        std::cerr << "Failed to install Ctrl+C handler.\n";
        SEMILIVE_LOG_ERROR("failed to install Ctrl+C handler");
        return EXIT_FAILURE;
    }

    try {
        const auto result = run_publisher(options->publisher);
        SEMILIVE_LOG_INFO("publisher process stopped with exit code {}",
                          result);
        return result;
    } catch (const std::exception& error) {
        std::cerr << "Publisher terminated with an exception: "
                  << error.what() << '\n';
        SEMILIVE_LOG_CRITICAL("publisher terminated with an exception: {}",
                              error.what());
    } catch (...) {
        std::cerr << "Publisher terminated with an unknown exception.\n";
        SEMILIVE_LOG_CRITICAL(
            "publisher terminated with an unknown exception");
    }

    return EXIT_FAILURE;
}
