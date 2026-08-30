#include "common/infrastructure/log/log.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

#define SEMILIVE_LOG_TAG "publisher_main"

namespace {

void print_help() {
    std::cout << "SemiLive publisher (project skeleton)\n"
                 "Usage: semilive_publisher [--help|--version]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view argument{argv[1]};
        if (argument == "--help") {
            print_help();
            return EXIT_SUCCESS;
        }
        if (argument == "--version") {
            std::cout << "semilive_publisher 0.1.0-dev\n";
            return EXIT_SUCCESS;
        }
    }

    if (argc != 1) {
        std::cerr << "Unknown arguments. Use --help for usage.\n";
        return EXIT_FAILURE;
    }

    semilive::log::Config log_config;
    log_config.file_path = "logs/semilive_publisher.log";
    const auto log_result = semilive::log::init(log_config);
    if (log_result == semilive::log::InitResult::Failed) {
        std::cerr << "Failed to initialize logging.\n";
        return EXIT_FAILURE;
    }

    SEMILIVE_LOG_INFO("publisher process started");
    std::cout << "SemiLive publisher skeleton: implementation pending.\n";
    SEMILIVE_LOG_INFO("publisher process stopped");
    semilive::log::shutdown();
    return EXIT_SUCCESS;
}
