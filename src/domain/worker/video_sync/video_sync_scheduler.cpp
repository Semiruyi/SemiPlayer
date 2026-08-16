#include "domain/worker/video_sync/video_sync_scheduler.hpp"

#include <utility>

namespace semi::domain {
namespace {

void absorb_input_result(const VideoSyncInputResult& input_result,
                         VideoSyncScheduleResult& result) noexcept {
    result.stale_items_dropped += input_result.stale_items_dropped;
    if (input_result.frame_popped) {
        ++result.frames_popped;
    }
    if (input_result.kind == VideoSyncInputResultKind::Empty) {
        result.empty_pop = true;
    }
}

} // namespace

VideoSyncScheduleResult VideoFrameScheduler::step(
    VideoSyncInput& input,
    VideoSyncClock& clock,
    Generation::Value current_generation,
    bool playback_enabled) noexcept {
    VideoSyncScheduleResult result;
    const auto clock_snapshot = clock.snapshot();

    if (clock_snapshot.external_clock_required && !clock_snapshot.pts_us) {
        result.audio_clock_unavailable = true;

        if (!pending_frame_) {
            auto input_result = input.try_pop_current(current_generation);
            absorb_input_result(input_result, result);
            if (input_result.kind == VideoSyncInputResultKind::Frame) {
                pending_frame_ = std::move(input_result.frame);
            } else {
                return result;
            }
        }

        waiting_for_audio_position_ = true;
        next_presentation_deadline_.reset();
        return result;
    }

    std::optional<std::int64_t> clock_pts = clock_snapshot.pts_us;
    std::optional<RenderedVideoFrame> candidate;
    if (pending_frame_) {
        if (!frame_is_due(*pending_frame_,
                          clock,
                          clock_pts,
                          playback_enabled,
                          result)) {
            return result;
        }

        if (next_presentation_deadline_) {
            const auto overshoot = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - *next_presentation_deadline_);
            if (overshoot.count() > 0) {
                result.wait_overshoot_observed = true;
                result.wait_overshoot_us = static_cast<std::uint64_t>(overshoot.count());
            }
        }

        candidate.emplace(std::move(*pending_frame_));
        pending_frame_.reset();
        next_presentation_deadline_.reset();
    }

    const bool can_drain_multiple = playback_enabled;
    if (!candidate || can_drain_multiple) {
        for (;;) {
            auto input_result = input.try_pop_current(current_generation);
            absorb_input_result(input_result, result);
            if (input_result.kind != VideoSyncInputResultKind::Frame) {
                break;
            }

            if (!input_result.frame) {
                break;
            }
            auto frame = std::move(*input_result.frame);
            if (!frame_is_due(frame,
                              clock,
                              clock_pts,
                              playback_enabled,
                              result)) {
                pending_frame_ = std::move(frame);
                break;
            }

            if (candidate) {
                ++result.catchup_drops;
            }
            candidate.emplace(std::move(frame));
            if (!can_drain_multiple) {
                break;
            }
        }
    }

    result.presentation_clock_pts_us = clock_pts;
    result.frame = std::move(candidate);
    return result;
}

void VideoFrameScheduler::reset(bool paused_generation_pending) noexcept {
    pending_frame_.reset();
    next_presentation_deadline_.reset();
    paused_generation_pending_ = paused_generation_pending;
    waiting_for_audio_position_ = false;
    waiting_for_resume_ = false;
}

void VideoFrameScheduler::on_generation_changed(bool playback_enabled) noexcept {
    reset(!playback_enabled);
}

void VideoFrameScheduler::on_playback_started() noexcept {
    paused_generation_pending_ = false;
    waiting_for_audio_position_ = false;
    waiting_for_resume_ = false;
    next_presentation_deadline_.reset();
}

void VideoFrameScheduler::on_playback_paused() noexcept {
    waiting_for_audio_position_ = false;
    waiting_for_resume_ = false;
    next_presentation_deadline_.reset();
}

void VideoFrameScheduler::on_audio_position_ready() noexcept {
    waiting_for_audio_position_ = false;
}

void VideoFrameScheduler::on_audio_playback_finished() noexcept {
    waiting_for_audio_position_ = false;
    next_presentation_deadline_.reset();
}

void VideoFrameScheduler::on_frame_presented(bool playback_enabled) noexcept {
    if (!playback_enabled) {
        paused_generation_pending_ = false;
    }
}

bool VideoFrameScheduler::has_pending_frame() const noexcept {
    return pending_frame_.has_value();
}

bool VideoFrameScheduler::paused_generation_pending() const noexcept {
    return paused_generation_pending_;
}

bool VideoFrameScheduler::waiting_for_audio_position() const noexcept {
    return waiting_for_audio_position_;
}

bool VideoFrameScheduler::waiting_for_resume() const noexcept {
    return waiting_for_resume_;
}

std::optional<VideoFrameScheduler::Clock::time_point>
VideoFrameScheduler::next_presentation_deadline() const noexcept {
    return next_presentation_deadline_;
}

bool VideoFrameScheduler::frame_is_due(const RenderedVideoFrame& frame,
                                       VideoSyncClock& clock,
                                       std::optional<std::int64_t>& clock_pts,
                                       bool playback_enabled,
                                       VideoSyncScheduleResult& result) noexcept {
    // A paused clock cannot advance to a post-seek frame whose PTS is slightly
    // later than the first prepared audio PTS. Present that first frame once.
    if (!playback_enabled && paused_generation_pending_) {
        return true;
    }

    const auto frame_pts = frame.rendered().pts_us;
    clock_pts = clock.current_pts_for_frame(frame_pts, playback_enabled);
    if (!frame_pts || !clock_pts || *frame_pts <= *clock_pts) {
        return true;
    }

    result.wait_target_us = static_cast<std::uint64_t>(*frame_pts - *clock_pts);
    result.wait_scheduled = true;
    schedule_wait(*frame_pts, *clock_pts, playback_enabled);
    return false;
}

void VideoFrameScheduler::schedule_wait(std::int64_t frame_pts,
                                        std::int64_t clock_pts,
                                        bool playback_enabled) noexcept {
    if (!playback_enabled) {
        waiting_for_resume_ = true;
        next_presentation_deadline_.reset();
        return;
    }

    waiting_for_resume_ = false;
    waiting_for_audio_position_ = false;
    next_presentation_deadline_ =
        Clock::now() + std::chrono::microseconds(frame_pts - clock_pts);
}

} // namespace semi::domain
