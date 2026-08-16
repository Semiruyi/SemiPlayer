#pragma once

#include "domain/worker/video_sync/video_sync_telemetry.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace semi::domain {

struct VideoSyncMetricsSnapshot final {
    Generation::Value generation = 0;
    double elapsed_ms = 0.0;
    double fps = 0.0;

    std::uint64_t rendered_frames_popped = 0;
    std::uint64_t frames_presented = 0;
    std::uint64_t frames_dropped_for_catchup = 0;
    std::uint64_t stale_items_dropped = 0;
    std::uint64_t empty_pop_attempts = 0;
    std::uint64_t audio_clock_unavailable = 0;

    std::uint64_t wait_events = 0;
    std::uint64_t wait_target_total_us = 0;
    std::uint64_t max_wait_target_us = 0;
    double wait_target_average_us = 0.0;

    std::uint64_t wait_overshoot_events = 0;
    std::uint64_t wait_overshoot_total_us = 0;
    std::uint64_t max_wait_overshoot_us = 0;
    double wait_overshoot_average_us = 0.0;

    std::uint64_t wakeup_error_events = 0;
    std::int64_t wakeup_error_total_us = 0;
    std::uint64_t max_wakeup_lateness_us = 0;
    std::uint64_t max_wakeup_earliness_us = 0;
    double wakeup_error_average_us = 0.0;
    std::int64_t wakeup_compensation_us = 0;

    std::uint64_t busy_wait_events = 0;
    std::uint64_t busy_wait_total_us = 0;
    std::uint64_t max_busy_wait_us = 0;
    double busy_wait_average_us = 0.0;

    std::uint64_t presented_late_frames = 0;
    std::uint64_t presented_lateness_total_us = 0;
    std::uint64_t max_presented_lateness_us = 0;
    double presented_lateness_average_us = 0.0;

    std::uint64_t callback_duration_total_us = 0;
    std::uint64_t max_callback_duration_us = 0;
    double callback_duration_average_us = 0.0;
};

// Single-worker metrics accumulator. snapshot() is intended to be read after
// the observed session has stopped; it does not add synchronization to the
// VideoSync hot path.
class VideoSyncMetrics final : public VideoSyncTelemetry {
public:
    using Clock = std::chrono::steady_clock;

    void on_session_started(Generation::Value generation) noexcept override;
    void on_playback_started() noexcept override;
    void on_frame_popped() noexcept override;
    void on_frame_dropped_for_catchup() noexcept override;
    void on_stale_item_dropped() noexcept override;
    void on_empty_pop() noexcept override;
    void on_audio_clock_unavailable() noexcept override;
    void on_wait_scheduled(std::uint64_t target_us) noexcept override;
    void on_wait_overshoot(std::uint64_t overshoot_us) noexcept override;
    void on_wakeup_error(std::int64_t error_us,
                         std::int64_t compensation_us) noexcept override;
    void on_busy_wait(std::uint64_t duration_us) noexcept override;
    void on_frame_presented(
        const VideoSyncPresentationObservation& observation) noexcept override;
    void on_session_finished(std::string_view reason) noexcept override;

    [[nodiscard]] VideoSyncMetricsSnapshot snapshot() const noexcept;

private:
    void reset() noexcept;
    void log_snapshot(std::string_view reason) const noexcept;

    Generation::Value generation_ = 0;
    std::optional<Clock::time_point> playback_started_at_;

    std::uint64_t rendered_frames_popped_ = 0;
    std::uint64_t frames_presented_ = 0;
    std::uint64_t frames_dropped_for_catchup_ = 0;
    std::uint64_t stale_items_dropped_ = 0;
    std::uint64_t empty_pop_attempts_ = 0;
    std::uint64_t audio_clock_unavailable_ = 0;

    std::uint64_t wait_events_ = 0;
    std::uint64_t wait_target_total_us_ = 0;
    std::uint64_t max_wait_target_us_ = 0;
    std::uint64_t wait_overshoot_events_ = 0;
    std::uint64_t wait_overshoot_total_us_ = 0;
    std::uint64_t max_wait_overshoot_us_ = 0;

    std::uint64_t wakeup_error_events_ = 0;
    std::int64_t wakeup_error_total_us_ = 0;
    std::uint64_t max_wakeup_lateness_us_ = 0;
    std::uint64_t max_wakeup_earliness_us_ = 0;
    std::int64_t wakeup_compensation_us_ = 0;

    std::uint64_t busy_wait_events_ = 0;
    std::uint64_t busy_wait_total_us_ = 0;
    std::uint64_t max_busy_wait_us_ = 0;

    std::uint64_t presented_late_frames_ = 0;
    std::uint64_t presented_lateness_total_us_ = 0;
    std::uint64_t max_presented_lateness_us_ = 0;

    std::uint64_t callback_duration_total_us_ = 0;
    std::uint64_t max_callback_duration_us_ = 0;
};

} // namespace semi::domain
