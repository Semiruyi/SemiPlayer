#pragma once

#include "domain/resource/generation/generation.hpp"
#include "domain/worker/video_sync/video_sync_clock.hpp"
#include "domain/worker/video_sync/video_sync_input.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace semi::domain {

struct VideoSyncScheduleResult final {
    std::optional<RenderedVideoFrame> frame;
    std::uint64_t stale_items_dropped = 0;
    std::uint64_t frames_popped = 0;
    std::uint64_t catchup_drops = 0;
    std::uint64_t wait_target_us = 0;
    std::uint64_t wait_overshoot_us = 0;
    bool empty_pop = false;
    bool audio_clock_unavailable = false;
    bool wait_scheduled = false;
    bool wait_overshoot_observed = false;
    std::optional<std::int64_t> presentation_clock_pts_us;
};

// Chooses which rendered frame should be presented next. It owns only
// worker-thread scheduling state: the pending frame, catch-up policy and
// presentation deadline. It does not know about Notifier, host callbacks or
// metrics. The wakeup controller may wait before this deadline to compensate
// for timer latency.
class VideoFrameScheduler final {
public:
    using Clock = std::chrono::steady_clock;

    VideoFrameScheduler() = default;

    VideoFrameScheduler(const VideoFrameScheduler&) = delete;
    VideoFrameScheduler& operator=(const VideoFrameScheduler&) = delete;
    VideoFrameScheduler(VideoFrameScheduler&&) = delete;
    VideoFrameScheduler& operator=(VideoFrameScheduler&&) = delete;

    [[nodiscard]] VideoSyncScheduleResult
    step(VideoSyncInput& input,
         VideoSyncClock& clock,
         Generation::Value current_generation,
         bool playback_enabled) noexcept;

    void reset(bool paused_generation_pending = false) noexcept;
    void on_generation_changed(bool playback_enabled) noexcept;
    void on_playback_started() noexcept;
    void on_playback_paused() noexcept;
    void on_audio_position_ready() noexcept;
    void on_audio_playback_finished() noexcept;
    void on_frame_presented(bool playback_enabled) noexcept;

    [[nodiscard]] bool has_pending_frame() const noexcept;
    [[nodiscard]] bool paused_generation_pending() const noexcept;
    [[nodiscard]] bool waiting_for_audio_position() const noexcept;
    [[nodiscard]] bool waiting_for_resume() const noexcept;
    [[nodiscard]] std::optional<Clock::time_point>
    next_presentation_deadline() const noexcept;

private:
    [[nodiscard]] bool frame_is_due(const RenderedVideoFrame& frame,
                                    VideoSyncClock& clock,
                                    std::optional<std::int64_t>& clock_pts,
                                    bool playback_enabled,
                                    VideoSyncScheduleResult& result) noexcept;
    void schedule_wait(std::int64_t frame_pts,
                       std::int64_t clock_pts,
                       bool playback_enabled) noexcept;

    std::optional<RenderedVideoFrame> pending_frame_;
    std::optional<Clock::time_point> next_presentation_deadline_;
    bool paused_generation_pending_ = false;
    bool waiting_for_audio_position_ = false;
    bool waiting_for_resume_ = false;
};

} // namespace semi::domain
