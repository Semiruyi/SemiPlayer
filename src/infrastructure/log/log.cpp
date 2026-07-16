#include "infrastructure/log/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/fmt/chrono.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace semi::log {
namespace {

enum class State {
    Uninitialized,
    Running,
    ConsoleOnly,
    ShuttingDown,
    Stopped,
};

using LoggerPtr = std::shared_ptr<spdlog::logger>;

std::atomic<State> g_state{State::Uninitialized};
std::atomic<Level> g_min_level{Level::Info};
std::atomic<Level> g_fallback_level{Level::Info};
std::atomic<LoggerPtr> g_logger{};
std::mutex g_lifecycle_mutex;
std::mutex g_stderr_mutex;

constexpr char kLoggerName[] = "semi_player";
constexpr std::size_t kDefaultQueueSize = 8192;
constexpr std::size_t kDefaultWorkerThreads = 1;
constexpr std::size_t kDefaultRotateBytes = 10 * 1024 * 1024;
constexpr std::size_t kDefaultRotateFiles = 3;
constexpr std::string_view kDefaultFilePath = "logs/semi_player.log";
constexpr std::string_view kPattern = "%Y-%m-%d %H:%M:%S.%e [%^%l%$] [tid %t] %v";

int to_rank(Level level) noexcept {
    return static_cast<int>(level);
}

spdlog::level::level_enum to_spdlog_level(Level level) noexcept {
    switch (level) {
    case Level::Trace:
        return spdlog::level::trace;
    case Level::Debug:
        return spdlog::level::debug;
    case Level::Info:
        return spdlog::level::info;
    case Level::Warn:
        return spdlog::level::warn;
    case Level::Error:
        return spdlog::level::err;
    case Level::Critical:
        return spdlog::level::critical;
    case Level::Off:
        return spdlog::level::off;
    }

    return spdlog::level::off;
}

spdlog::async_overflow_policy to_overflow_policy(OverflowPolicy policy) noexcept {
    switch (policy) {
    case OverflowPolicy::Block:
        return spdlog::async_overflow_policy::block;
    case OverflowPolicy::OverrunOldest:
        return spdlog::async_overflow_policy::overrun_oldest;
    }

    return spdlog::async_overflow_policy::overrun_oldest;
}

std::string_view level_name(Level level) noexcept {
    switch (level) {
    case Level::Trace:
        return "TRACE";
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO";
    case Level::Warn:
        return "WARN";
    case Level::Error:
        return "ERROR";
    case Level::Critical:
        return "CRITICAL";
    case Level::Off:
        return "OFF";
    }

    return "UNKNOWN";
}

Config normalize_config(const Config& config) {
    Config normalized = config;
    if (normalized.file_path.empty()) {
        normalized.file_path = std::string{kDefaultFilePath};
    }
    if (normalized.queue_size == 0) {
        normalized.queue_size = kDefaultQueueSize;
    }
    if (normalized.worker_threads == 0) {
        normalized.worker_threads = kDefaultWorkerThreads;
    }
    if (normalized.rotate_bytes == 0) {
        normalized.rotate_bytes = kDefaultRotateBytes;
    }
    if (normalized.rotate_files == 0) {
        normalized.rotate_files = kDefaultRotateFiles;
    }
    return normalized;
}

std::string_view base_name(std::string_view path) noexcept {
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

std::tm local_time(std::time_t value) noexcept {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &value);
#else
    localtime_r(&value, &tm);
#endif
    return tm;
}

std::string render_payload(std::string_view tag,
                           const std::source_location& location,
                           std::string_view message) {
    return fmt::format(
        "[{}] [{}:{}] {}",
        tag,
        base_name(location.file_name()),
        location.line(),
        message);
}

void raw_stderr_write(Level level,
                      std::string_view tag,
                      const std::source_location& location,
                      std::string_view message) noexcept {
    try {
        const auto now = std::chrono::system_clock::now();
        const auto tt = std::chrono::system_clock::to_time_t(now);
        const auto millis =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) %
            1000;
        const auto local = local_time(tt);
        const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        const auto line = fmt::format(
            "{}.{:03d} [{}] [tid {}] [{}] [{}:{}] {}\n",
            fmt::format("{:%Y-%m-%d %H:%M:%S}", local),
            static_cast<int>(millis.count()),
            level_name(level),
            tid,
            tag,
            base_name(location.file_name()),
            location.line(),
            message);

        std::lock_guard<std::mutex> lock(g_stderr_mutex);
        fmt::print(stderr, "{}", line);
        std::fflush(stderr);
    } catch (...) {
        std::lock_guard<std::mutex> lock(g_stderr_mutex);
        std::fputs("logger fallback write failed\n", stderr);
        std::fflush(stderr);
    }
}

bool running_state(State state) noexcept {
    return state == State::Running || state == State::ConsoleOnly;
}

} // namespace

InitResult init(const Config& config) noexcept {
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);

    const auto state = g_state.load();
    if (state == State::Running || state == State::ConsoleOnly || state == State::ShuttingDown) {
        return InitResult::AlreadyInitialized;
    }

    const Config normalized = normalize_config(config);
    g_min_level.store(normalized.level);
    g_fallback_level.store(normalized.console_level);

    try {
        spdlog::init_thread_pool(normalized.queue_size, normalized.worker_threads);

        auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        stderr_sink->set_level(to_spdlog_level(normalized.console_level));

        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(stderr_sink);

        InitResult result = InitResult::ConsoleOnly;
        if (!normalized.file_path.empty()) {
            try {
                const std::filesystem::path log_path{normalized.file_path};
                if (log_path.has_parent_path()) {
                    std::filesystem::create_directories(log_path.parent_path());
                }

                auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    normalized.file_path,
                    normalized.rotate_bytes,
                    normalized.rotate_files,
                    false);
                file_sink->set_level(to_spdlog_level(normalized.level));
                sinks.insert(sinks.begin(), file_sink);
                result = InitResult::Ready;
            } catch (...) {
                result = InitResult::ConsoleOnly;
            }
        }

        auto logger = std::make_shared<spdlog::async_logger>(
            kLoggerName,
            sinks.begin(),
            sinks.end(),
            spdlog::thread_pool(),
            to_overflow_policy(normalized.overflow));
        logger->set_level(to_spdlog_level(normalized.level));
        logger->set_pattern(std::string{kPattern});
        logger->flush_on(spdlog::level::err);

        g_logger.store(logger);
        g_state.store(result == InitResult::Ready ? State::Running : State::ConsoleOnly);
        return result;
    } catch (const std::exception& ex) {
        detail::report_internal_failure("logger init failed", ex.what());
    } catch (...) {
        detail::report_internal_failure("logger init failed", "unknown exception");
    }

    g_logger.store(nullptr);
    spdlog::shutdown();
    g_state.store(State::Stopped);
    return InitResult::Failed;
}

void shutdown() noexcept {
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);

    const auto state = g_state.load();
    if (state == State::Uninitialized || state == State::Stopped) {
        g_state.store(State::Stopped);
        return;
    }

    g_state.store(State::ShuttingDown);

    auto logger = g_logger.exchange(nullptr);
    if (logger) {
        try {
            logger->flush();
        } catch (const std::exception& ex) {
            detail::report_internal_failure("logger flush during shutdown failed", ex.what());
        } catch (...) {
            detail::report_internal_failure(
                "logger flush during shutdown failed",
                "unknown exception");
        }
    }

    spdlog::shutdown();
    g_state.store(State::Stopped);
}

void flush() noexcept {
    auto logger = g_logger.load();
    if (!logger) {
        return;
    }

    try {
        logger->flush();
    } catch (const std::exception& ex) {
        detail::report_internal_failure("logger flush failed", ex.what());
    } catch (...) {
        detail::report_internal_failure("logger flush failed", "unknown exception");
    }
}

namespace detail {

bool should_log(Level level) noexcept {
    const auto state = g_state.load();
    if (running_state(state)) {
        return to_rank(level) >= to_rank(g_min_level.load());
    }

    return to_rank(level) >= to_rank(g_fallback_level.load());
}

void write_formatted(Level level,
                     std::string_view tag,
                     const std::source_location& location,
                     std::string_view message) noexcept {
    try {
        const auto state = g_state.load();
        if (running_state(state)) {
            auto logger = g_logger.load();
            if (logger) {
                logger->log(to_spdlog_level(level), "{}", render_payload(tag, location, message));
                return;
            }
        }

        raw_stderr_write(level, tag, location, message);
    } catch (const std::exception& ex) {
        report_internal_failure("logger write failed", ex.what());
    } catch (...) {
        report_internal_failure("logger write failed", "unknown exception");
    }
}

void report_internal_failure(std::string_view context, std::string_view detail) noexcept {
    try {
        raw_stderr_write(
            Level::Error,
            "Logger",
            std::source_location::current(),
            fmt::format("{}: {}", context, detail));
    } catch (...) {
        std::lock_guard<std::mutex> lock(g_stderr_mutex);
        std::fputs("logger internal failure\n", stderr);
        std::fflush(stderr);
    }
}

} // namespace detail
} // namespace semi::log
