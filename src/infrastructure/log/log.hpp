#pragma once

#include <cstddef>
#include <exception>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/fmt/fmt.h>

namespace semi::log {

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
    std::string file_path = "logs/semi_player.log";
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
void report_internal_failure(std::string_view context,
                             std::string_view detail) noexcept;

} // namespace detail

inline void write(Level level,
                  std::string_view tag,
                  const std::source_location& location,
                  std::string_view message) noexcept {
    if (!detail::should_log(level)) {
        return;
    }

    detail::write_formatted(level, tag, location, message);
}

template <typename... Args>
inline void write(Level level,
                  std::string_view tag,
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
    } catch (const std::exception& ex) {
        detail::report_internal_failure("formatting failed", ex.what());
    } catch (...) {
        detail::report_internal_failure("formatting failed", "unknown exception");
    }
}

} // namespace semi::log

#define SEMI_LOG_TRACE(...) ::semi::log::write(::semi::log::Level::Trace, SEMI_LOG_TAG, std::source_location::current(), __VA_ARGS__)
#define SEMI_LOG_DEBUG(...) ::semi::log::write(::semi::log::Level::Debug, SEMI_LOG_TAG, std::source_location::current(), __VA_ARGS__)
#define SEMI_LOG_INFO(...) ::semi::log::write(::semi::log::Level::Info, SEMI_LOG_TAG, std::source_location::current(), __VA_ARGS__)
#define SEMI_LOG_WARN(...) ::semi::log::write(::semi::log::Level::Warn, SEMI_LOG_TAG, std::source_location::current(), __VA_ARGS__)
#define SEMI_LOG_ERROR(...) ::semi::log::write(::semi::log::Level::Error, SEMI_LOG_TAG, std::source_location::current(), __VA_ARGS__)
#define SEMI_LOG_CRITICAL(...) ::semi::log::write(::semi::log::Level::Critical, SEMI_LOG_TAG, std::source_location::current(), __VA_ARGS__)
