#include "domain/worker/video_sync/video_sync_metrics.hpp"

#include "infrastructure/log/log.hpp"

#include <algorithm>

#define SEMI_LOG_TAG "video_sync_metrics"

namespace semi::domain {

void VideoSyncMetrics::on_session_started(Generation::Value generation) noexcept {
    reset();
    generation_ = generation;
}

void VideoSyncMetrics::on_playback_started() noexcept {
    if (!playback_started_at_) {
        playback_started_at_ = Clock::now();
    }
}

void VideoSyncMetrics::on_frame_popped() noexcept {
    ++rendered_frames_popped_;
}

void VideoSyncMetrics::on_frame_dropped_for_catchup() noexcept {
    ++frames_dropped_for_catchup_;
}

void VideoSyncMetrics::on_stale_item_dropped() noexcept {
    ++stale_items_dropped_;
}

void VideoSyncMetrics::on_empty_pop() noexcept {
    ++empty_pop_attempts_;
}

void VideoSyncMetrics::on_audio_clock_unavailable() noexcept {
    ++audio_clock_unavailable_;
}

void VideoSyncMetrics::on_wait_scheduled(std::uint64_t target_us) noexcept {
    ++wait_events_;
    wait_target_total_us_ += target_us;
    max_wait_target_us_ = std::max(max_wait_target_us_, target_us);
}

void VideoSyncMetrics::on_wait_overshoot(std::uint64_t overshoot_us) noexcept {
    if (overshoot_us == 0) {
        return;
    }

    ++wait_overshoot_events_;
    wait_overshoot_total_us_ += overshoot_us;
    max_wait_overshoot_us_ = std::max(max_wait_overshoot_us_, overshoot_us);
}

void VideoSyncMetrics::on_frame_presented(
    const VideoSyncPresentationObservation& observation) noexcept {
    ++frames_presented_;
    callback_duration_total_us_ += observation.callback_duration_us;
    max_callback_duration_us_ =
        std::max(max_callback_duration_us_, observation.callback_duration_us);

    if (observation.frame_pts_us && observation.clock_pts_us &&
        *observation.clock_pts_us > *observation.frame_pts_us) {
        const auto lateness_us = static_cast<std::uint64_t>(
            *observation.clock_pts_us - *observation.frame_pts_us);
        ++presented_late_frames_;
        presented_lateness_total_us_ += lateness_us;
        max_presented_lateness_us_ =
            std::max(max_presented_lateness_us_, lateness_us);
    }
}

void VideoSyncMetrics::on_session_finished(std::string_view reason) noexcept {
    log_snapshot(reason);
}

VideoSyncMetricsSnapshot VideoSyncMetrics::snapshot() const noexcept {
    VideoSyncMetricsSnapshot result{
        .generation = generation_,
        .rendered_frames_popped = rendered_frames_popped_,
        .frames_presented = frames_presented_,
        .frames_dropped_for_catchup = frames_dropped_for_catchup_,
        .stale_items_dropped = stale_items_dropped_,
        .empty_pop_attempts = empty_pop_attempts_,
        .audio_clock_unavailable = audio_clock_unavailable_,
        .wait_events = wait_events_,
        .wait_target_total_us = wait_target_total_us_,
        .max_wait_target_us = max_wait_target_us_,
        .wait_overshoot_events = wait_overshoot_events_,
        .wait_overshoot_total_us = wait_overshoot_total_us_,
        .max_wait_overshoot_us = max_wait_overshoot_us_,
        .presented_late_frames = presented_late_frames_,
        .presented_lateness_total_us = presented_lateness_total_us_,
        .max_presented_lateness_us = max_presented_lateness_us_,
        .callback_duration_total_us = callback_duration_total_us_,
        .max_callback_duration_us = max_callback_duration_us_,
    };

    if (playback_started_at_) {
        result.elapsed_ms = std::chrono::duration<double, std::milli>(
                                Clock::now() - *playback_started_at_)
                                .count();
    }
    if (result.elapsed_ms > 0.0) {
        result.fps = static_cast<double>(result.frames_presented) * 1000.0 /
                     result.elapsed_ms;
    }
    if (result.wait_events > 0) {
        result.wait_target_average_us =
            static_cast<double>(result.wait_target_total_us) /
            static_cast<double>(result.wait_events);
    }
    if (result.wait_overshoot_events > 0) {
        result.wait_overshoot_average_us =
            static_cast<double>(result.wait_overshoot_total_us) /
            static_cast<double>(result.wait_overshoot_events);
    }
    if (result.presented_late_frames > 0) {
        result.presented_lateness_average_us =
            static_cast<double>(result.presented_lateness_total_us) /
            static_cast<double>(result.presented_late_frames);
    }
    if (result.frames_presented > 0) {
        result.callback_duration_average_us =
            static_cast<double>(result.callback_duration_total_us) /
            static_cast<double>(result.frames_presented);
    }
    return result;
}

void VideoSyncMetrics::reset() noexcept {
    generation_ = 0;
    playback_started_at_.reset();
    rendered_frames_popped_ = 0;
    frames_presented_ = 0;
    frames_dropped_for_catchup_ = 0;
    stale_items_dropped_ = 0;
    empty_pop_attempts_ = 0;
    audio_clock_unavailable_ = 0;
    wait_events_ = 0;
    wait_target_total_us_ = 0;
    max_wait_target_us_ = 0;
    wait_overshoot_events_ = 0;
    wait_overshoot_total_us_ = 0;
    max_wait_overshoot_us_ = 0;
    presented_late_frames_ = 0;
    presented_lateness_total_us_ = 0;
    max_presented_lateness_us_ = 0;
    callback_duration_total_us_ = 0;
    max_callback_duration_us_ = 0;
}

void VideoSyncMetrics::log_snapshot(std::string_view reason) const noexcept {
    const auto metrics = snapshot();
    SEMI_LOG_INFO(
        "presentation stats reason={} generation={} elapsed_ms={:.3f} fps={:.3f} "
        "popped={} presented={} catchup_dropped={} stale_dropped={} empty_pop={} "
        "audio_clock_unavailable={} wait_events={} wait_target_avg_us={:.3f} "
        "wait_target_max_us={} wait_overshoot_avg_us={:.3f} "
        "wait_overshoot_max_us={} presented_late={} "
        "presented_lateness_avg_us={:.3f} presented_lateness_max_us={} "
        "callback_avg_us={:.3f} callback_max_us={}",
        reason,
        metrics.generation,
        metrics.elapsed_ms,
        metrics.fps,
        metrics.rendered_frames_popped,
        metrics.frames_presented,
        metrics.frames_dropped_for_catchup,
        metrics.stale_items_dropped,
        metrics.empty_pop_attempts,
        metrics.audio_clock_unavailable,
        metrics.wait_events,
        metrics.wait_target_average_us,
        metrics.max_wait_target_us,
        metrics.wait_overshoot_average_us,
        metrics.max_wait_overshoot_us,
        metrics.presented_late_frames,
        metrics.presented_lateness_average_us,
        metrics.max_presented_lateness_us,
        metrics.callback_duration_average_us,
        metrics.max_callback_duration_us);
}

} // namespace semi::domain
