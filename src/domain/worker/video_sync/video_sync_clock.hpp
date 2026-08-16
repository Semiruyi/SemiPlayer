#pragma once

#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_output/audio_output.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace semi::domain {

struct VideoSyncClockSnapshot final {
    std::optional<std::int64_t> pts_us;
    bool external_clock_required = false;
};

// Provides the clock used by VideoSync. Audio is the master when available;
// after audio finishes, or for video-only media, the clock falls back to a
// monotonic local clock. This object is owned and mutated by the VideoSync
// worker thread only.
class VideoSyncClock final {
public:
    explicit VideoSyncClock(std::shared_ptr<AudioOutput> audio_output) noexcept;

    VideoSyncClock(const VideoSyncClock&) = delete;
    VideoSyncClock& operator=(const VideoSyncClock&) = delete;
    VideoSyncClock(VideoSyncClock&&) = delete;
    VideoSyncClock& operator=(VideoSyncClock&&) = delete;

    [[nodiscard]] bool has_audio_output() const noexcept;
    [[nodiscard]] bool audio_playback_finished() const noexcept;

    void configure(bool audio_master, Generation::Value generation) noexcept;
    void on_generation_changed(Generation::Value generation) noexcept;
    void on_audio_playback_finished(bool playback_enabled) noexcept;

    [[nodiscard]] VideoSyncClockSnapshot snapshot() const noexcept;
    [[nodiscard]] std::optional<std::int64_t>
    current_pts_for_frame(std::optional<std::int64_t> frame_pts_us,
                          bool playback_enabled) noexcept;

    void pause() noexcept;
    void resume() noexcept;
    void reset() noexcept;

private:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] bool external_clock_required() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> current_pts() const noexcept;
    void anchor_local_clock_if_needed(std::int64_t pts_us,
                                      bool playback_enabled) noexcept;

    std::shared_ptr<AudioOutput> audio_output_;
    bool audio_master_ = true;
    Generation::Value active_generation_ = 0;
    bool audio_playback_finished_ = false;

    std::optional<Clock::time_point> local_clock_started_at_;
    std::int64_t local_clock_start_pts_us_ = 0;
    std::optional<std::int64_t> local_clock_frozen_pts_us_;
    bool local_clock_paused_ = true;
};

} // namespace semi::domain
