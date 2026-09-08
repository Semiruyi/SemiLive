#pragma once

#include <cstddef>
#include <exception>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/fmt/fmt.h>

namespace semilive::log {

enum class Level {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5,
    Off = 6,
};

enum class OverflowPolicy {
    Block,
    OverrunOldest,
};

enum class InitResult {
    Ready,
    ConsoleOnly,
    AlreadyInitialized,
    Failed,
};

struct Config {
    std::string file_path = "logs/semilive.log";
    Level level = Level::Info;
    Level console_level = Level::Warn;
    std::size_t queue_size = 8192;
    std::size_t worker_threads = 1;
    OverflowPolicy overflow = OverflowPolicy::OverrunOldest;
    std::size_t rotate_bytes = 10 * 1024 * 1024;
    std::size_t rotate_files = 3;
};

InitResult init(const Config& config) noexcept;
void shutdown() noexcept;
void flush() noexcept;

namespace detail {

bool should_log(Level level) noexcept;
void write_formatted(Level level,
                     std::string_view tag,
                     const std::source_location& location,
                     std::string_view message) noexcept;
void report_internal_failure(std::string_view context, std::string_view detail) noexcept;

}  // namespace detail

inline void write(const Level level,
                  const std::string_view tag,
                  const std::source_location& location,
                  const std::string_view message) noexcept {
    if (!detail::should_log(level)) {
        return;
    }

    detail::write_formatted(level, tag, location, message);
}

template <typename... Args>
inline void write(const Level level,
                  const std::string_view tag,
                  const std::source_location& location,
                  fmt::format_string<Args...> format,
                  Args&&... args) noexcept {
    if (!detail::should_log(level)) {
        return;
    }

    try {
        detail::write_formatted(
            level,
            tag,
            location,
            fmt::format(format, std::forward<Args>(args)...));
    } catch (const std::exception& exception) {
        detail::report_internal_failure("formatting failed", exception.what());
    } catch (...) {
        detail::report_internal_failure("formatting failed", "unknown exception");
    }
}

}  // namespace semilive::log

#define SEMILIVE_LOG_TRACE(...)                                                                  \
    ::semilive::log::write(::semilive::log::Level::Trace,                                        \
                           SEMILIVE_LOG_TAG,                                                      \
                           std::source_location::current(),                                       \
                           __VA_ARGS__)
#define SEMILIVE_LOG_DEBUG(...)                                                                  \
    ::semilive::log::write(::semilive::log::Level::Debug,                                        \
                           SEMILIVE_LOG_TAG,                                                      \
                           std::source_location::current(),                                       \
                           __VA_ARGS__)
#define SEMILIVE_LOG_INFO(...)                                                                   \
    ::semilive::log::write(::semilive::log::Level::Info,                                         \
                           SEMILIVE_LOG_TAG,                                                      \
                           std::source_location::current(),                                       \
                           __VA_ARGS__)
#define SEMILIVE_LOG_WARN(...)                                                                   \
    ::semilive::log::write(::semilive::log::Level::Warn,                                         \
                           SEMILIVE_LOG_TAG,                                                      \
                           std::source_location::current(),                                       \
                           __VA_ARGS__)
#define SEMILIVE_LOG_ERROR(...)                                                                  \
    ::semilive::log::write(::semilive::log::Level::Error,                                        \
                           SEMILIVE_LOG_TAG,                                                      \
                           std::source_location::current(),                                       \
                           __VA_ARGS__)
#define SEMILIVE_LOG_CRITICAL(...)                                                               \
    ::semilive::log::write(::semilive::log::Level::Critical,                                     \
                           SEMILIVE_LOG_TAG,                                                      \
                           std::source_location::current(),                                       \
                           __VA_ARGS__)
