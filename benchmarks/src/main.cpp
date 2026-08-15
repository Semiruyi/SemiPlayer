#include "semi_player/semi_player.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kFrameWaitTimeout = std::chrono::seconds(10);
constexpr auto kPausedFrameSettleTime = std::chrono::milliseconds(100);
constexpr auto kProcessSamplePeriod = std::chrono::milliseconds(250);

const char* status_name(int status) noexcept {
    switch (status) {
    case SEMI_OK:
        return "SEMI_OK";
    case SEMI_ERR_NOT_INITIALIZED:
        return "SEMI_ERR_NOT_INITIALIZED";
    case SEMI_ERR_INVALID_STATE:
        return "SEMI_ERR_INVALID_STATE";
    case SEMI_ERR_CANCELLED:
        return "SEMI_ERR_CANCELLED";
    case SEMI_ERR_ASSEMBLE_FAILED:
        return "SEMI_ERR_ASSEMBLE_FAILED";
    case SEMI_ERR_INTERNAL:
        return "SEMI_ERR_INTERNAL";
    case SEMI_ERR_INVALID_ARGUMENT:
        return "SEMI_ERR_INVALID_ARGUMENT";
    case SEMI_ERR_INVALID_HANDLE:
        return "SEMI_ERR_INVALID_HANDLE";
    case SEMI_ERR_INVALID_RESOURCE:
        return "SEMI_ERR_INVALID_RESOURCE";
    default:
        return "SEMI_STATUS_UNKNOWN";
    }
}

double milliseconds_since(Clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct FrameSnapshot final {
    std::uint64_t count = 0;
    std::uint32_t generation = 0;
    std::int64_t pts_us = 0;
    bool has_pts = false;
};

class FrameObserver final {
public:
    static void callback(void* user_data, const semi_video_frame_t* frame) noexcept {
        if (user_data == nullptr || frame == nullptr) {
            return;
        }
        static_cast<FrameObserver*>(user_data)->on_frame(*frame);
    }

    bool wait_for_count(std::uint64_t minimum_count,
                        std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] {
            return snapshot_locked().count >= minimum_count;
        });
    }

    bool wait_for_new_generation(std::uint64_t previous_count,
                                 std::uint32_t previous_generation,
                                 std::chrono::milliseconds timeout,
                                 FrameSnapshot& result) {
        std::unique_lock lock(mutex_);
        const bool received = condition_.wait_for(lock, timeout, [&] {
            return count_ > previous_count && generation_ > previous_generation;
        });
        result = snapshot_locked();
        return received;
    }

    FrameSnapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        return snapshot_locked();
    }

private:
    void on_frame(const semi_video_frame_t& frame) noexcept {
        {
            std::scoped_lock lock(mutex_);
            ++count_;
            generation_ = frame.generation;
            has_pts_ = frame.has_pts != 0U;
            pts_us_ = frame.pts_us;
        }
        condition_.notify_all();
    }

    FrameSnapshot snapshot_locked() const noexcept {
        return FrameSnapshot{
            .count = count_,
            .generation = generation_,
            .pts_us = pts_us_,
            .has_pts = has_pts_,
        };
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::uint64_t count_ = 0;
    std::uint32_t generation_ = 0;
    std::int64_t pts_us_ = 0;
    bool has_pts_ = false;
};

bool await_command(std::string_view name,
                   semi_handle_t handle,
                   semi_command_result_t* result = nullptr) {
    if (handle == 0U) {
        std::cerr << "[benchmark] " << name << " returned no handle\n";
        return false;
    }

    semi_command_result_t local_result{};
    const int status = semi_player_handle_await(
        handle, result != nullptr ? result : &local_result);
    if (status != SEMI_OK) {
        std::cerr << "[benchmark] " << name << " failed: " << status_name(status)
                  << " (" << status << ")\n";
        return false;
    }
    return true;
}

class PlayerSession final {
public:
    PlayerSession() = default;

    ~PlayerSession() {
        close_and_shutdown();
    }

    PlayerSession(const PlayerSession&) = delete;
    PlayerSession& operator=(const PlayerSession&) = delete;

    bool initialize() {
        if (semi_player_init() != SEMI_OK) {
            std::cerr << "[benchmark] semi_player_init failed\n";
            return false;
        }
        initialized_ = true;

        semi_video_output_config_t config{};
        config.struct_size = sizeof(config);
        config.pixel_format = SEMI_VIDEO_PIXEL_FORMAT_RGBA8888;
        config.on_frame = &FrameObserver::callback;
        config.user_data = &frames_;
        if (!await_command("configure_video_output",
                           semi_player_configure_video_output(&config))) {
            return false;
        }
        return true;
    }

    bool open(const std::string& media_path, semi_media_info_t& media_info) {
        if (!await_command("open", semi_player_open(media_path.c_str()),
                           &open_result_)) {
            return false;
        }
        media_info = open_result_.media_info;
        opened_ = true;
        return true;
    }

    bool play() { return await_command("play", semi_player_play()); }

    bool pause() { return await_command("pause", semi_player_pause()); }

    bool seek(std::int64_t position_us) {
        return await_command(
            "seek",
            semi_player_seek(position_us, SEMI_SEEK_MODE_PREVIOUS_KEYFRAME));
    }

    bool close() {
        if (!opened_) {
            return true;
        }
        const bool succeeded = await_command("close", semi_player_close());
        if (succeeded) {
            opened_ = false;
        }
        return succeeded;
    }

    void close_and_shutdown() noexcept {
        close();
        if (initialized_) {
            semi_player_shutdown();
            initialized_ = false;
        }
    }

    FrameObserver& frames() noexcept { return frames_; }

private:
    FrameObserver frames_;
    semi_command_result_t open_result_{};
    bool initialized_ = false;
    bool opened_ = false;
};

struct ProcessMetrics final {
    double cpu_average_percent = -1.0;
    double cpu_p95_percent = -1.0;
    std::uint64_t peak_working_set_bytes = 0;
};

std::uint64_t filetime_value(const FILETIME& time) noexcept {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

class ProcessSampler final {
public:
    void start() noexcept {
        last_wall_ = Clock::now();
        last_cpu_ = process_cpu_time();
        sample_memory();
    }

    void sample() noexcept {
        const auto now = Clock::now();
        const auto cpu = process_cpu_time();
        const double wall_seconds =
            std::chrono::duration<double>(now - last_wall_).count();
        if (last_cpu_ != 0U && cpu >= last_cpu_ && wall_seconds > 0.0) {
            const double cpu_seconds =
                static_cast<double>(cpu - last_cpu_) / 10'000'000.0;
            cpu_samples_.push_back(cpu_seconds / wall_seconds * 100.0);
        }
        last_wall_ = now;
        last_cpu_ = cpu;
        sample_memory();
    }

    ProcessMetrics finish() noexcept {
        sample();
        ProcessMetrics result;
        if (!cpu_samples_.empty()) {
            std::sort(cpu_samples_.begin(), cpu_samples_.end());
            double sum = 0.0;
            for (const double value : cpu_samples_) {
                sum += value;
            }
            result.cpu_average_percent = sum / cpu_samples_.size();
            const auto index = static_cast<std::size_t>(
                std::ceil(0.95 * static_cast<double>(cpu_samples_.size())));
            result.cpu_p95_percent =
                cpu_samples_[std::min(index == 0U ? 0U : index - 1U,
                                      cpu_samples_.size() - 1U)];
        }
        result.peak_working_set_bytes = peak_working_set_bytes_;
        return result;
    }

private:
    static std::uint64_t process_cpu_time() noexcept {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel,
                             &user)) {
            return 0U;
        }
        return filetime_value(kernel) + filetime_value(user);
    }

    void sample_memory() noexcept {
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters,
                                 sizeof(counters)) != FALSE) {
            peak_working_set_bytes_ =
                std::max<std::uint64_t>(peak_working_set_bytes_,
                                        counters.WorkingSetSize);
        }
    }

    Clock::time_point last_wall_{};
    std::uint64_t last_cpu_ = 0U;
    std::vector<double> cpu_samples_;
    std::uint64_t peak_working_set_bytes_ = 0U;
};

struct BenchmarkRow final {
    std::string scenario;
    int run = 0;
    double seek_fraction = -1.0;
    double open_to_first_frame_ms = -1.0;
    double seek_to_first_frame_ms = -1.0;
    std::int64_t target_pts_us = -1;
    std::int64_t first_frame_pts_us = -1;
    std::int64_t seek_error_us = -1;
    bool paused_after_seek = false;
    std::uint64_t frames = 0U;
    double elapsed_ms = -1.0;
    double cpu_average_percent = -1.0;
    double cpu_p95_percent = -1.0;
    std::uint64_t peak_working_set_bytes = 0U;
    int status = SEMI_OK;
};

std::string csv_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 2U);
    escaped.push_back('"');
    for (const char character : value) {
        if (character == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

template <typename T>
std::string csv_number(T value) {
    if constexpr (std::is_floating_point_v<T>) {
        if (value < 0.0) {
            return {};
        }
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(3) << value;
        return stream.str();
    } else {
        if (value < 0) {
            return {};
        }
        return std::to_string(value);
    }
}

std::string csv_signed_microseconds(std::int64_t value) {
    return value == -1 ? std::string{} : std::to_string(value);
}

class CsvWriter final {
public:
    explicit CsvWriter(const std::filesystem::path& output_path) {
        if (!output_path.parent_path().empty()) {
            std::filesystem::create_directories(output_path.parent_path());
        }
        output_.open(output_path, std::ios::out | std::ios::trunc);
        if (!output_) {
            throw std::runtime_error("cannot open benchmark output: " +
                                     output_path.string());
        }
        output_ << "scenario,run,seek_fraction,open_to_first_frame_ms,"
                   "seek_to_first_frame_ms,target_pts_us,first_frame_pts_us,"
                   "seek_error_us,paused_after_seek,frames,elapsed_ms,"
                   "cpu_average_percent,cpu_p95_percent,"
                   "peak_working_set_bytes,status\n";
    }

    void write(const BenchmarkRow& row) {
        output_ << csv_escape(row.scenario) << ',' << row.run << ','
                 << csv_number(row.seek_fraction) << ','
                 << csv_number(row.open_to_first_frame_ms) << ','
                 << csv_number(row.seek_to_first_frame_ms) << ','
                 << csv_number(row.target_pts_us) << ','
                 << csv_number(row.first_frame_pts_us) << ','
                 << csv_signed_microseconds(row.seek_error_us) << ','
                 << (row.paused_after_seek ? "1" : "0") << ','
                 << csv_number(row.frames) << ',' << csv_number(row.elapsed_ms)
                 << ',' << csv_number(row.cpu_average_percent) << ','
                 << csv_number(row.cpu_p95_percent) << ','
                 << csv_number(row.peak_working_set_bytes) << ',' << row.status
                 << '\n';
        output_.flush();
    }

private:
    std::ofstream output_;
};

struct Options final {
    std::filesystem::path media_path;
    std::string scenario = "all";
    std::filesystem::path output_path = "benchmark-results.csv";
    int runs = 1;
    int warmups = 0;
    int steady_seconds = 60;
};

void print_usage(const char* executable) {
    std::cout << "Usage: " << executable
              << " --media <path> [options]\n\n"
                 "Options:\n"
                 "  --scenario <all|startup|paused-seek|steady>\n"
                 "  --output <csv-path>       default: benchmark-results.csv\n"
                 "  --runs <count>            default: 1\n"
                 "  --warmups <count>         default: 0\n"
                 "  --steady-seconds <count>  default: 60\n"
                 "  --help\n";
}

int parse_nonnegative_int(std::string_view option, std::string_view value) {
    try {
        const int parsed = std::stoi(std::string(value));
        if (parsed < 0) {
            throw std::invalid_argument("must be non-negative");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string(option) +
                                    " requires a positive integer");
    }
}

int parse_positive_int(std::string_view option, std::string_view value) {
    const int parsed = parse_nonnegative_int(option, value);
    if (parsed == 0) {
        throw std::invalid_argument(std::string(option) +
                                    " requires a positive integer");
    }
    return parsed;
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        auto require_value = [&](std::string_view option) -> std::string_view {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string(option) +
                                            " requires a value");
            }
            return argv[++index];
        };

        if (argument == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        }
        if (argument == "--media") {
            options.media_path = require_value(argument);
        } else if (argument == "--scenario") {
            options.scenario = require_value(argument);
            if (options.scenario != "all" && options.scenario != "startup" &&
                options.scenario != "paused-seek" && options.scenario != "steady") {
                throw std::invalid_argument("unknown scenario: " + options.scenario);
            }
        } else if (argument == "--output") {
            options.output_path = require_value(argument);
        } else if (argument == "--runs") {
            options.runs = parse_positive_int(argument, require_value(argument));
        } else if (argument == "--warmups") {
            options.warmups =
                parse_nonnegative_int(argument, require_value(argument));
        } else if (argument == "--steady-seconds") {
            options.steady_seconds =
                parse_positive_int(argument, require_value(argument));
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }

    if (options.media_path.empty()) {
        throw std::invalid_argument("--media is required");
    }
    if (!std::filesystem::is_regular_file(options.media_path)) {
        throw std::invalid_argument("media file does not exist: " +
                                    options.media_path.string());
    }
    return options;
}

bool prepare_session(PlayerSession& session,
                     const std::filesystem::path& media_path,
                     semi_media_info_t& media_info) {
    return session.initialize() &&
           session.open(media_path.string(), media_info);
}

std::optional<BenchmarkRow> run_startup(const Options& options, int run) {
    PlayerSession session;
    semi_media_info_t media_info{};
    if (!session.initialize()) {
        return std::nullopt;
    }

    const auto open_start = Clock::now();
    if (!session.open(options.media_path.string(), media_info) ||
        !session.play() ||
        !session.frames().wait_for_count(1, kFrameWaitTimeout)) {
        return std::nullopt;
    }

    BenchmarkRow row;
    row.scenario = "startup";
    row.run = run;
    row.open_to_first_frame_ms = milliseconds_since(open_start);
    row.frames = session.frames().snapshot().count;
    row.status = SEMI_OK;
    return row;
}

std::optional<BenchmarkRow> run_paused_seek(const Options& options,
                                            int run,
                                            double seek_fraction) {
    PlayerSession session;
    semi_media_info_t media_info{};
    if (!prepare_session(session, options.media_path, media_info) ||
        !session.play() ||
        !session.frames().wait_for_count(1, kFrameWaitTimeout) ||
        !session.pause()) {
        return std::nullopt;
    }

    const FrameSnapshot before_seek = session.frames().snapshot();
    const auto target_pts_us = static_cast<std::int64_t>(
        static_cast<double>(media_info.duration_us) * seek_fraction);
    const auto seek_start = Clock::now();
    if (!session.seek(target_pts_us)) {
        return std::nullopt;
    }

    FrameSnapshot after_seek;
    if (!session.frames().wait_for_new_generation(
            before_seek.count, before_seek.generation, kFrameWaitTimeout,
            after_seek)) {
        std::cerr << "[benchmark] paused seek did not produce a new generation\n";
        return std::nullopt;
    }

    const auto count_after_seek = after_seek.count;
    std::this_thread::sleep_for(kPausedFrameSettleTime);
    const FrameSnapshot settled = session.frames().snapshot();

    BenchmarkRow row;
    row.scenario = "paused_seek";
    row.run = run;
    row.seek_fraction = seek_fraction;
    row.seek_to_first_frame_ms = milliseconds_since(seek_start);
    row.target_pts_us = target_pts_us;
    row.first_frame_pts_us = after_seek.has_pts ? after_seek.pts_us : -1;
    if (after_seek.has_pts) {
    row.seek_error_us = after_seek.pts_us - target_pts_us;
    }
    row.paused_after_seek = settled.count == count_after_seek;
    row.frames = settled.count - before_seek.count;
    row.status = SEMI_OK;
    return row;
}

std::optional<BenchmarkRow> run_steady_playback(const Options& options, int run) {
    PlayerSession session;
    semi_media_info_t media_info{};
    if (!prepare_session(session, options.media_path, media_info) ||
        !session.play() ||
        !session.frames().wait_for_count(1, kFrameWaitTimeout)) {
        return std::nullopt;
    }

    const double media_seconds =
        static_cast<double>(media_info.duration_us) / 1'000'000.0;
    const double requested_seconds = static_cast<double>(options.steady_seconds);
    const double run_seconds =
        std::max(0.25, std::min(requested_seconds, media_seconds - 0.25));

    const auto initial_frame_count = session.frames().snapshot().count;
    ProcessSampler sampler;
    sampler.start();
    const auto playback_start = Clock::now();
    while (std::chrono::duration<double>(Clock::now() - playback_start).count() <
           run_seconds) {
        std::this_thread::sleep_for(kProcessSamplePeriod);
        sampler.sample();
    }
    const ProcessMetrics metrics = sampler.finish();

    BenchmarkRow row;
    row.scenario = "steady_playback";
    row.run = run;
    row.elapsed_ms = milliseconds_since(playback_start);
    row.frames = session.frames().snapshot().count - initial_frame_count;
    row.cpu_average_percent = metrics.cpu_average_percent;
    row.cpu_p95_percent = metrics.cpu_p95_percent;
    row.peak_working_set_bytes = metrics.peak_working_set_bytes;
    row.status = SEMI_OK;
    return row;
}

template <typename Function>
bool run_repeated(const Options& options,
                  CsvWriter& writer,
                  Function&& function) {
    const int total_runs = options.warmups + options.runs;
    for (int index = 1; index <= total_runs; ++index) {
        const bool is_warmup = index <= options.warmups;
        const int measured_run = index - options.warmups;
        const auto row = function(measured_run);
        if (!row) {
            return false;
        }
        if (!is_warmup) {
            writer.write(*row);
            std::cout << "[benchmark] " << row->scenario << " run "
                      << measured_run << " completed\n";
        }
    }
    return true;
}

bool run_all(const Options& options, CsvWriter& writer) {
    if (options.scenario == "all" || options.scenario == "startup") {
        if (!run_repeated(options, writer, [&](int run) {
                return run_startup(options, run);
            })) {
            return false;
        }
    }

    if (options.scenario == "all" || options.scenario == "paused-seek") {
        constexpr std::array<double, 3> kSeekFractions{0.25, 0.50, 0.75};
        for (const double fraction : kSeekFractions) {
            if (!run_repeated(options, writer, [&](int run) {
                    return run_paused_seek(options, run, fraction);
                })) {
                return false;
            }
        }
    }

    if (options.scenario == "all" || options.scenario == "steady") {
        if (!run_repeated(options, writer, [&](int run) {
                return run_steady_playback(options, run);
            })) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        CsvWriter writer(options.output_path);
        if (!run_all(options, writer)) {
            std::cerr << "[benchmark] benchmark failed\n";
            return 1;
        }
        std::cout << "[benchmark] results: " << options.output_path.string()
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[benchmark] error: " << error.what() << '\n';
        return 2;
    }
}
