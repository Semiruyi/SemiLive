#include <semilive/common/log/log.hpp>
#include <semilive/relay/application/relay_session.hpp>
#include <semilive/relay/reporting/relay_session_report.hpp>

#include <charconv>
#include <chrono>
#include <csignal>
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

#define SEMILIVE_LOG_TAG "relay_main"

namespace {

namespace application = semilive::relay::application;
namespace domain = semilive::relay::domain;
namespace reporting = semilive::relay::reporting;

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void handle_stop_signal(int) {
    stop_requested = 1;
}

enum class CommandLineAction { Run, Help, Version };

struct CommandLineOptions {
    CommandLineAction action = CommandLineAction::Run;
    application::RelayConfig relay;
    std::optional<std::filesystem::path> stats_json_path;
};

using CommandLineResult =
    std::expected<CommandLineOptions, std::string>;

class LogLifetime final {
public:
    ~LogLifetime() { semilive::log::shutdown(); }
    LogLifetime(const LogLifetime&) = delete;
    LogLifetime& operator=(const LogLifetime&) = delete;
    LogLifetime() = default;
};

void print_help() {
    std::cout
        << "SemiLive UDP impairment relay\n"
           "Usage: semilive_relay [options]\n\n"
           "Options:\n"
           "  --bind-address ADDRESS          Numeric input IPv4 or IPv6\n"
           "                                  address (default: 127.0.0.1)\n"
           "  --bind-port PORT                Input UDP port (required)\n"
           "  --forward-address ADDRESS       Numeric destination IPv4 or IPv6\n"
           "                                  address (default: 127.0.0.1)\n"
           "  --forward-port PORT             Destination UDP port (required)\n"
           "  --loss-percent PERCENT          Random datagram loss in 0..100\n"
           "                                  (default: 0, precision: 0.0001)\n"
           "  --seed VALUE                    Random seed (default: 1)\n"
           "  --max-datagram-bytes SIZE       Accepted UDP payload limit\n"
           "                                  (default: 65507)\n"
           "  --receive-buffer-bytes SIZE     Requested kernel receive buffer\n"
           "                                  (default: 4194304)\n"
           "  --poll-interval-ms MS           Stop polling interval in 1..1000\n"
           "                                  (default: 10)\n"
           "  --stats-json PATH               Write final session statistics\n"
           "                                  as JSON (existing file replaced)\n"
           "  --help                          Show this help\n"
           "  --version                       Show the version\n";
}

template <typename Integer>
std::expected<Integer, std::string> parse_integer(
    const std::string_view text, const std::string_view option,
    const Integer minimum, const Integer maximum) {
    std::uint64_t parsed = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() ||
        parsed < static_cast<std::uint64_t>(minimum) ||
        parsed > static_cast<std::uint64_t>(maximum)) {
        return std::unexpected{
            std::string{option} + " requires an integer in " +
            std::to_string(minimum) + ".." + std::to_string(maximum)};
    }
    return static_cast<Integer>(parsed);
}

std::expected<std::uint32_t, std::string> parse_loss_rate_ppm(
    const std::string_view text) {
    constexpr std::string_view error_message =
        "--loss-percent requires a decimal in 0..100 with at most "
        "four fractional digits";
    if (text.empty()) {
        return std::unexpected{std::string{error_message}};
    }
    const auto decimal = text.find('.');
    if (decimal != std::string_view::npos &&
        text.find('.', decimal + 1) != std::string_view::npos) {
        return std::unexpected{std::string{error_message}};
    }
    const auto whole_text = text.substr(0, decimal);
    const auto fraction_text = decimal == std::string_view::npos
                                   ? std::string_view{}
                                   : text.substr(decimal + 1);
    if (whole_text.empty() || fraction_text.size() > 4) {
        return std::unexpected{std::string{error_message}};
    }

    auto whole = parse_integer<std::uint32_t>(whole_text,
                                               "--loss-percent", 0, 100);
    if (!whole) {
        return std::unexpected{std::string{error_message}};
    }
    std::uint32_t fraction = 0;
    if (!fraction_text.empty()) {
        const auto [end, error] = std::from_chars(
            fraction_text.data(),
            fraction_text.data() + fraction_text.size(), fraction);
        if (error != std::errc{} ||
            end != fraction_text.data() + fraction_text.size()) {
            return std::unexpected{std::string{error_message}};
        }
        for (std::size_t index = fraction_text.size(); index < 4; ++index) {
            fraction *= 10;
        }
    }
    const auto result = *whole * 10'000U + fraction;
    if (result > domain::RandomLossPolicy::rate_scale) {
        return std::unexpected{std::string{error_message}};
    }
    return result;
}

CommandLineResult parse_command_line(const int argc, char* argv[]) {
    CommandLineOptions options;
    bool bind_address_set = false;
    bool bind_port_set = false;
    bool forward_address_set = false;
    bool forward_port_set = false;
    bool loss_set = false;
    bool seed_set = false;
    bool maximum_datagram_set = false;
    bool receive_buffer_set = false;
    bool poll_interval_set = false;
    bool stats_json_set = false;

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

        const auto read_value = [&]()
            -> std::expected<std::string_view, std::string> {
            if (index + 1 >= argc) {
                return std::unexpected{
                    std::string{argument} + " requires a value"};
            }
            return std::string_view{argv[++index]};
        };
        const auto reject_duplicate = [&](bool& seen)
            -> std::expected<void, std::string> {
            if (seen) {
                return std::unexpected{
                    std::string{argument} +
                    " may only be specified once"};
            }
            seen = true;
            return {};
        };

        if (argument == "--bind-address") {
            if (auto unique = reject_duplicate(bind_address_set); !unique) {
                return std::unexpected{std::move(unique.error())};
            }
            auto value = read_value();
            if (!value) return std::unexpected{std::move(value.error())};
            options.relay.network.bind_address = *value;
            continue;
        }
        if (argument == "--bind-port") {
            if (auto unique = reject_duplicate(bind_port_set); !unique) {
                return std::unexpected{std::move(unique.error())};
            }
            auto value = read_value();
            if (!value) return std::unexpected{std::move(value.error())};
            auto parsed = parse_integer<std::uint16_t>(
                *value, argument, 1, 65'535);
            if (!parsed) return std::unexpected{std::move(parsed.error())};
            options.relay.network.bind_port = *parsed;
            continue;
        }
        if (argument == "--forward-address") {
            if (auto unique = reject_duplicate(forward_address_set);
                !unique) {
                return std::unexpected{std::move(unique.error())};
            }
            auto value = read_value();
            if (!value) return std::unexpected{std::move(value.error())};
            options.relay.network.forward_address = *value;
            continue;
        }
        if (argument == "--forward-port") {
            if (auto unique = reject_duplicate(forward_port_set); !unique) {
                return std::unexpected{std::move(unique.error())};
            }
            auto value = read_value();
            if (!value) return std::unexpected{std::move(value.error())};
            auto parsed = parse_integer<std::uint16_t>(
                *value, argument, 1, 65'535);
            if (!parsed) return std::unexpected{std::move(parsed.error())};
            options.relay.network.forward_port = *parsed;
            continue;
        }
        if (argument == "--loss-percent") {
            if (auto unique = reject_duplicate(loss_set); !unique) {
                return std::unexpected{std::move(unique.error())};
            }
            auto value = read_value();
            if (!value) return std::unexpected{std::move(value.error())};
            auto parsed = parse_loss_rate_ppm(*value);
            if (!parsed) return std::unexpected{std::move(parsed.error())};
            options.relay.loss_rate_ppm = *parsed;
            continue;
        }
        if (argument == "--seed") {
            if (auto unique = reject_duplicate(seed_set); !unique) {
                return std::unexpected{std::move(unique.error())};
            }
            auto value = read_value();
            if (!value) return std::unexpected{std::move(value.error())};
            auto parsed = parse_integer<std::uint64_t>(
                *value, argument, std::uint64_t{0},
                std::numeric_limits<std::uint64_t>::max());
            if (!parsed) return std::unexpected{std::move(parsed.error())};
            options.relay.random_seed = *parsed;
            continue;
        }
        if (argument == "--max-datagram-bytes") {
            if (auto unique = reject_duplicate(maximum_datagram_set);
                !unique) {
                return std::unexpected{std::move(unique.error())};
            }
            auto value = read_value();
            if (!value) return std::unexpected{std::move(value.error())};
            auto parsed = parse_integer<std::size_t>(
                *value, argument, std::size_t{1}, std::size_t{65'507});
            if (!parsed) return std::unexpected{std::move(parsed.error())};
            options.relay.network.maximum_datagram_bytes = *parsed;
            continue;
        }
        if (argument == "--receive-buffer-bytes") {
            if (auto unique = reject_duplicate(receive_buffer_set); !unique) {
                return std::unexpected{std::move(unique.error())};
            }
            auto value = read_value();
            if (!value) return std::unexpected{std::move(value.error())};
            auto parsed = parse_integer<std::size_t>(
                *value, argument, std::size_t{1},
                static_cast<std::size_t>(
                    std::numeric_limits<int>::max()));
            if (!parsed) return std::unexpected{std::move(parsed.error())};
            options.relay.network.receive_buffer_bytes = *parsed;
            continue;
        }
        if (argument == "--poll-interval-ms") {
            if (auto unique = reject_duplicate(poll_interval_set); !unique) {
                return std::unexpected{std::move(unique.error())};
            }
            auto value = read_value();
            if (!value) return std::unexpected{std::move(value.error())};
            auto parsed = parse_integer<std::int64_t>(
                *value, argument, std::int64_t{1}, std::int64_t{1'000});
            if (!parsed) return std::unexpected{std::move(parsed.error())};
            options.relay.poll_interval = std::chrono::milliseconds{*parsed};
            continue;
        }
        if (argument == "--stats-json") {
            if (auto unique = reject_duplicate(stats_json_set); !unique) {
                return std::unexpected{std::move(unique.error())};
            }
            auto value = read_value();
            if (!value) return std::unexpected{std::move(value.error())};
            if (value->empty()) {
                return std::unexpected{"--stats-json requires a path"};
            }
            options.stats_json_path = std::filesystem::path{*value};
            continue;
        }
        return std::unexpected{"unknown argument: " +
                               std::string{argument}};
    }

    if (!bind_port_set || !forward_port_set) {
        return std::unexpected{
            "--bind-port and --forward-port are required"};
    }
    if (options.relay.network.bind_address ==
            options.relay.network.forward_address &&
        options.relay.network.bind_port ==
            options.relay.network.forward_port) {
        return std::unexpected{
            "input and forward UDP endpoints must be different"};
    }
    return options;
}

int run_relay(application::RelayConfig config,
              const std::optional<std::filesystem::path>& stats_json_path) {
    const auto report_config = config;
    application::RelaySession session{std::move(config)};
    auto opened = session.open();
    if (!opened) {
        std::cerr << "Relay start failed: " << opened.error() << '\n';
        SEMILIVE_LOG_ERROR("relay start failed: {}", opened.error());
        return EXIT_FAILURE;
    }

    std::cout << "Relaying UDP datagrams\n"
              << "  input: " << opened->bound_address << ':'
              << opened->bound_port << '\n'
              << "  output: " << report_config.network.forward_address
              << ':' << report_config.network.forward_port << '\n'
              << "  random loss: "
              << static_cast<double>(report_config.loss_rate_ppm) / 10'000.0
              << "% (seed " << report_config.random_seed << ")\n"
              << "Press Ctrl+C to stop.\n";
    SEMILIVE_LOG_INFO(
        "relay started: input={}:{}, output={}:{}, loss_ppm={}, seed={}",
        opened->bound_address, opened->bound_port,
        report_config.network.forward_address,
        report_config.network.forward_port, report_config.loss_rate_ppm,
        report_config.random_seed);

    const auto started_at = std::chrono::steady_clock::now();
    bool succeeded = true;
    while (stop_requested == 0) {
        const auto result = session.poll_once();
        if (!result) {
            std::cerr << "Relay failed: " << result.error() << '\n';
            SEMILIVE_LOG_ERROR("relay failed: {}", result.error());
            succeeded = false;
            break;
        }
    }
    const auto duration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_at);
    session.close();

    const auto stats = session.stats();
    std::cout << "Relay statistics\n"
              << "  received: " << stats.received_datagrams << " datagrams, "
              << stats.received_bytes << " bytes\n"
              << "  forwarded: " << stats.forwarded_datagrams
              << " datagrams, " << stats.forwarded_bytes << " bytes\n"
              << "  dropped: " << stats.dropped_datagrams << " datagrams, "
              << stats.dropped_bytes << " bytes\n";

    if (stats_json_path) {
        const reporting::RelaySessionReport report{
            report_config, *opened, stats, duration, succeeded};
        const auto written = reporting::write_relay_session_report_json(
            *stats_json_path, report);
        if (!written) {
            std::cerr << written.error() << '\n';
            SEMILIVE_LOG_ERROR("{}", written.error());
            succeeded = false;
        } else {
            std::cout << "  stats: " << stats_json_path->string() << '\n';
        }
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
        std::cout << "semilive_relay 0.1.0-dev\n";
        return EXIT_SUCCESS;
    }

    semilive::log::Config log_config;
    log_config.file_path = "logs/semilive_relay.log";
    if (semilive::log::init(log_config) ==
        semilive::log::InitResult::Failed) {
        std::cerr << "Failed to initialize logging.\n";
        return EXIT_FAILURE;
    }
    const LogLifetime log_lifetime;
    if (std::signal(SIGINT, handle_stop_signal) == SIG_ERR ||
        std::signal(SIGTERM, handle_stop_signal) == SIG_ERR) {
        std::cerr << "Failed to install relay stop handlers.\n";
        return EXIT_FAILURE;
    }

    try {
        return run_relay(options->relay, options->stats_json_path);
    } catch (const std::exception& error) {
        std::cerr << "Relay terminated with an exception: "
                  << error.what() << '\n';
        SEMILIVE_LOG_CRITICAL("relay terminated with an exception: {}",
                              error.what());
    } catch (...) {
        std::cerr << "Relay terminated with an unknown exception.\n";
        SEMILIVE_LOG_CRITICAL(
            "relay terminated with an unknown exception");
    }
    return EXIT_FAILURE;
}
