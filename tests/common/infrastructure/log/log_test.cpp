#include "common/infrastructure/log/log.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#define SEMILIVE_LOG_TAG "LoggerTest"

namespace {

using semilive::log::Config;
using semilive::log::InitResult;
using semilive::log::Level;
using semilive::log::OverflowPolicy;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

std::filesystem::path make_log_path(const std::string_view suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("semilive_logger_" + std::to_string(stamp) + "_" + std::string{suffix} + ".log");
}

Config make_config(const std::filesystem::path& path) {
    Config config;
    config.file_path = path.string();
    config.level = Level::Info;
    config.console_level = Level::Off;
    config.queue_size = 4096;
    config.worker_threads = 1;
    config.overflow = OverflowPolicy::Block;
    config.rotate_bytes = 1024 * 1024;
    config.rotate_files = 2;
    return config;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input{path};
    require(input.is_open(), "log file was not created");
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void remove_log_files(const std::filesystem::path& path) noexcept {
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + ".1", error);
    std::filesystem::remove(path.string() + ".2", error);
}

void writes_formatted_message_and_metadata() {
    const auto path = make_log_path("write");
    require(semilive::log::init(make_config(path)) == InitResult::Ready,
            "logger did not initialize");

    SEMILIVE_LOG_INFO("message {}", 42);
    semilive::log::shutdown();

    const auto content = read_file(path);
    require(content.contains("message 42"), "formatted message is missing");
    require(content.contains("[LoggerTest]"), "log tag is missing");
    require(content.contains("[log_test.cpp:"), "source location is missing");
    remove_log_files(path);
}

void rejects_repeated_initialization_until_shutdown() {
    const auto first_path = make_log_path("first");
    const auto second_path = make_log_path("second");

    require(semilive::log::init(make_config(first_path)) == InitResult::Ready,
            "first initialization failed");
    require(semilive::log::init(make_config(second_path)) == InitResult::AlreadyInitialized,
            "repeated initialization was not rejected");
    semilive::log::shutdown();
    require(semilive::log::init(make_config(second_path)) == InitResult::Ready,
            "initialization after shutdown failed");
    semilive::log::shutdown();

    remove_log_files(first_path);
    remove_log_files(second_path);
}

void accepts_concurrent_writers() {
    const auto path = make_log_path("concurrent");
    require(semilive::log::init(make_config(path)) == InitResult::Ready,
            "logger did not initialize for concurrent test");

    constexpr int kThreadCount = 4;
    constexpr int kMessagesPerThread = 100;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (int thread = 0; thread < kThreadCount; ++thread) {
        threads.emplace_back([thread] {
            for (int message = 0; message < kMessagesPerThread; ++message) {
                SEMILIVE_LOG_INFO("thread={} message={}", thread, message);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    semilive::log::shutdown();

    const auto content = read_file(path);
    for (int thread = 0; thread < kThreadCount; ++thread) {
        require(content.contains("thread=" + std::to_string(thread) + " message=0"),
                "a concurrent writer did not reach the log");
        require(content.contains("thread=" + std::to_string(thread) + " message=99"),
                "a concurrent writer did not finish logging");
    }
    remove_log_files(path);
}

}  // namespace

int main() {
    try {
        writes_formatted_message_and_metadata();
        rejects_repeated_initialization_until_shutdown();
        accepts_concurrent_writers();
        std::cout << "log tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        semilive::log::shutdown();
        std::cerr << "log test failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
