#include "infrastructure/statistics/performance_statistics.hpp"

#include "domain/worker/video_decoder/video_decoder_statistics.hpp"
#include "infrastructure/log/log.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>

#define SEMI_LOG_TAG "performance_statistics"

namespace semi::infra {
namespace {
constexpr std::size_t kQueueCapacity = 64;

std::filesystem::path default_path() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::filesystem::path("logs/performance") /
        ("statistics-" + std::to_string(stamp) + "-" + std::to_string(sequence++) + ".jsonl");
}

// JSON escaping includes control characters; codec names come from backends.
void quoted(std::ostream& stream, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    stream << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') stream << '\\' << static_cast<char>(c);
        else if (c < 0x20) stream << "\\u00" << hex[c >> 4] << hex[c & 15];
        else stream << static_cast<char>(c);
    }
    stream << '"';
}

std::string_view pixel_format(contracts::media::VideoPixelFormat format) noexcept {
    using enum contracts::media::VideoPixelFormat;
    switch (format) {
    case Yuv420p: return "yuv420p";
    case Yuv420p10le: return "yuv420p10le";
    case Yuv422p: return "yuv422p";
    case Yuv444p: return "yuv444p";
    case Nv12: return "nv12";
    case P010: return "p010";
    case Rgba8: return "rgba8";
    case Unknown: return "unknown";
    }
    return "unknown";
}

std::string serialize(const domain::VideoDecoderStatistics& report) {
    using enum domain::VideoDecoderStatisticsReason;
    const auto reason = report.reason == BackendFailure ? "backend_failure" :
        report.reason == Shutdown ? "shutdown" : "unconfigure";
    const auto calls = report.decode_calls + report.drain_calls;
    const auto total_ms = report.processing_total_ns / 1e6;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(6)
        << "{\"schema_version\":1,\"module\":\"video_decoder\",\"reason\":\"" << reason
        << "\",\"codec\":";
    quoted(out, report.codec);
    out << ",\"coded_width\":" << report.coded_width
        << ",\"coded_height\":" << report.coded_height
        << ",\"configured_at_unix_ms\":" << report.configured_at_unix_ms
        << ",\"finished_at_unix_ms\":" << report.finished_at_unix_ms
        << ",\"configured_duration_ms\":" << report.configured_duration_ns / 1e6
        << ",\"decode_calls\":" << report.decode_calls
        << ",\"drain_calls\":" << report.drain_calls
        << ",\"failed_calls\":" << report.failed_calls
        << ",\"output_frames\":" << report.output_frames
        << ",\"processing_total_ms\":" << total_ms
        << ",\"processing_max_ms\":" << report.processing_max_ns / 1e6
        << ",\"processing_mean_call_ms\":";
    if (calls) out << total_ms / calls; else out << "null";
    out << ",\"processing_per_output_frame_ms\":";
    if (report.output_frames) out << total_ms / report.output_frames; else out << "null";
    out << ",\"output_format\":";
    if (report.output_observed) quoted(out, pixel_format(report.output_format)); else out << "null";
    out << ",\"output_width\":";
    if (report.output_observed) out << report.output_width; else out << "null";
    out << ",\"output_height\":";
    if (report.output_observed) out << report.output_height; else out << "null";
    out << ",\"format_changed\":" << (report.format_changed ? "true" : "false") << '}';
    return out.str();
}
} // namespace

struct PerformanceStatistics::State {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<domain::VideoDecoderStatistics> reports;
    bool stopping = false;
    std::uint64_t dropped = 0;
};

PerformanceStatistics::PerformanceStatistics(std::shared_ptr<Notifier> notifier,
                                             std::filesystem::path output_path)
    : state_(std::make_shared<State>()) {
    if (!notifier) throw std::invalid_argument("statistics requires a notifier");
    if (output_path.empty()) output_path = default_path();
    decoder_subscription_ = notifier->subscribe<domain::VideoDecoderStatistics>(
        [state = state_](const domain::VideoDecoderStatistics& report) noexcept {
            // Capture state, not this: a synchronous notification already in
            // flight remains safe even if its subscription is removed.
            {
                std::lock_guard lock(state->mutex);
                if (state->stopping) return;
                if (state->reports.size() >= kQueueCapacity) {
                    ++state->dropped;
                    return;
                }
                try { state->reports.push_back(report); }
                catch (...) { ++state->dropped; return; }
            }
            state->cv.notify_one();
        });
    writer_ = std::thread([state = state_, path = std::move(output_path)] {
        write_reports(state, path);
    });
}

PerformanceStatistics::~PerformanceStatistics() {
    decoder_subscription_.reset();
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
    }
    state_->cv.notify_one();
    if (writer_.joinable()) writer_.join();
    if (state_->dropped) SEMI_LOG_WARN("dropped {} performance reports", state_->dropped);
}

void PerformanceStatistics::write_reports(const std::shared_ptr<State>& state,
                                           const std::filesystem::path& output_path) noexcept {
    std::ofstream file;
    std::uint64_t failed = 0;
    bool unavailable = false;
    for (;;) {
        domain::VideoDecoderStatistics report;
        {
            std::unique_lock lock(state->mutex);
            state->cv.wait(lock, [&] { return state->stopping || !state->reports.empty(); });
            if (state->reports.empty()) break;
            report = std::move(state->reports.front());
            state->reports.pop_front();
        }
        if (unavailable) { ++failed; continue; }
        try {
            if (!file.is_open()) {
                if (output_path.has_parent_path())
                    std::filesystem::create_directories(output_path.parent_path());
                file.exceptions(std::ios::failbit | std::ios::badbit);
                file.open(output_path, std::ios::out | std::ios::app | std::ios::binary);
                SEMI_LOG_INFO("performance reports: {}", output_path.string());
            }
            const auto line = serialize(report);
            file << line << '\n';
            file.flush();
        } catch (...) {
            ++failed;
            unavailable = true;
            SEMI_LOG_ERROR("performance report output failed: {}", output_path.string());
        }
    }
    if (failed) SEMI_LOG_WARN("failed to write {} performance reports", failed);
}

} // namespace semi::infra
