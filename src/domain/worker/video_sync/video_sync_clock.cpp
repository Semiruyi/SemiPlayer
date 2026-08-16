#include "domain/worker/video_sync/video_sync_clock.hpp"

#include <utility>

namespace semi::domain {

VideoSyncClock::VideoSyncClock(std::shared_ptr<AudioOutput> audio_output) noexcept
    : audio_output_(std::move(audio_output)) {}

bool VideoSyncClock::has_audio_output() const noexcept {
    return static_cast<bool>(audio_output_);
}

bool VideoSyncClock::audio_playback_finished() const noexcept {
    return audio_playback_finished_;
}

void VideoSyncClock::configure(bool audio_master, Generation::Value generation) noexcept {
    audio_master_ = audio_master;
    active_generation_ = generation;
    audio_playback_finished_ = false;
    reset();
}

void VideoSyncClock::on_generation_changed(Generation::Value generation) noexcept {
    active_generation_ = generation;
    audio_playback_finished_ = false;
    reset();
}

void VideoSyncClock::on_audio_playback_finished(bool playback_enabled) noexcept {
    reset();
    if (const auto position = audio_output_ ? audio_output_->current_position()
                                            : std::nullopt;
        position && position->generation == active_generation_) {
        local_clock_start_pts_us_ = position->pts_us;
        local_clock_started_at_ = Clock::now();
        local_clock_paused_ = !playback_enabled;
    }
    audio_playback_finished_ = true;
}

VideoSyncClockSnapshot VideoSyncClock::snapshot() const noexcept {
    return VideoSyncClockSnapshot{
        .pts_us = current_pts(),
        .external_clock_required = external_clock_required(),
    };
}

std::optional<std::int64_t>
VideoSyncClock::current_pts_for_frame(std::optional<std::int64_t> frame_pts_us,
                                      bool playback_enabled) noexcept {
    auto pts = current_pts();
    if (!pts && !external_clock_required() && frame_pts_us) {
        anchor_local_clock_if_needed(*frame_pts_us, playback_enabled);
        pts = current_pts();
    }
    return pts;
}

void VideoSyncClock::pause() noexcept {
    if (const auto current = current_pts();
        current && (!audio_master_ || audio_playback_finished_)) {
        local_clock_frozen_pts_us_ = *current;
    }
    local_clock_paused_ = true;
}

void VideoSyncClock::resume() noexcept {
    if (external_clock_required()) {
        return;
    }

    if (local_clock_frozen_pts_us_) {
        local_clock_start_pts_us_ = *local_clock_frozen_pts_us_;
        local_clock_started_at_ = Clock::now();
        local_clock_frozen_pts_us_.reset();
    }
    local_clock_paused_ = false;
}

void VideoSyncClock::reset() noexcept {
    local_clock_started_at_.reset();
    local_clock_start_pts_us_ = 0;
    local_clock_frozen_pts_us_.reset();
    local_clock_paused_ = true;
}

bool VideoSyncClock::external_clock_required() const noexcept {
    return audio_master_ && !audio_playback_finished_;
}

std::optional<std::int64_t> VideoSyncClock::current_pts() const noexcept {
    if (audio_master_) {
        if (audio_playback_finished_) {
            if (local_clock_frozen_pts_us_) {
                return *local_clock_frozen_pts_us_;
            }
            if (!local_clock_started_at_) {
                return std::nullopt;
            }
            return local_clock_start_pts_us_ +
                   std::chrono::duration_cast<std::chrono::microseconds>(
                       Clock::now() - *local_clock_started_at_)
                       .count();
        }

        if (!audio_output_) {
            return std::nullopt;
        }
        const auto position = audio_output_->current_position();
        if (!position || position->generation != active_generation_) {
            return std::nullopt;
        }
        return position->pts_us;
    }

    if (local_clock_paused_) {
        return local_clock_frozen_pts_us_;
    }
    if (!local_clock_started_at_) {
        return std::nullopt;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - *local_clock_started_at_);
    return local_clock_start_pts_us_ + elapsed.count();
}

void VideoSyncClock::anchor_local_clock_if_needed(std::int64_t pts_us,
                                                   bool playback_enabled) noexcept {
    if (local_clock_started_at_ || local_clock_frozen_pts_us_) {
        return;
    }

    local_clock_start_pts_us_ = pts_us;
    local_clock_started_at_ = Clock::now();
    if (!playback_enabled) {
        local_clock_frozen_pts_us_ = pts_us;
    }
}

} // namespace semi::domain
