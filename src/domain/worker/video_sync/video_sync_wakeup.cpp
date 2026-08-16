#include "domain/worker/video_sync/video_sync_wakeup.hpp"

#include <algorithm>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace semi::domain {

VideoSyncWakeupController::VideoSyncWakeupController(
    VideoSyncWakeupOptions options) noexcept {
    configure(options);
}

void VideoSyncWakeupController::configure(
    VideoSyncWakeupOptions options) noexcept {
    spin_window_ = std::max(options.spin_window, std::chrono::microseconds::zero());
    max_compensation_ =
        std::max(options.max_compensation, std::chrono::microseconds::zero());
    adaptive_ = options.adaptive;
    reset();
}

void VideoSyncWakeupController::reset() noexcept {
    clear_active_plan();
    error_window_.fill(0);
    error_count_ = 0;
    error_index_ = 0;
    error_sum_us_ = 0;
    compensation_us_ = 0;
}

void VideoSyncWakeupController::clear_active_plan() noexcept {
    active_presentation_deadline_.reset();
    active_wake_deadline_.reset();
    timer_wakeup_pending_ = false;
}

VideoSyncWakeupController::Clock::time_point
VideoSyncWakeupController::wake_deadline(
    Clock::time_point presentation_deadline) noexcept {
    if (!active_presentation_deadline_ ||
        *active_presentation_deadline_ != presentation_deadline) {
        active_presentation_deadline_ = presentation_deadline;
        const auto compensation = adaptive_
                                       ? std::chrono::microseconds(compensation_us_)
                                       : std::chrono::microseconds::zero();
        active_wake_deadline_ = presentation_deadline - spin_window_ - compensation;
        timer_wakeup_pending_ = false;
    }
    return *active_wake_deadline_;
}

bool VideoSyncWakeupController::timer_wakeup_pending(
    Clock::time_point presentation_deadline) const noexcept {
    return timer_wakeup_pending_ && active_presentation_deadline_ &&
           *active_presentation_deadline_ == presentation_deadline;
}

std::optional<VideoSyncWakeupObservation>
VideoSyncWakeupController::observe_timer_wakeup(
    Clock::time_point presentation_deadline,
    Clock::time_point actual_wakeup) noexcept {
    if (timer_wakeup_pending_ || !active_presentation_deadline_ ||
        !active_wake_deadline_ ||
        *active_presentation_deadline_ != presentation_deadline) {
        return std::nullopt;
    }

    const auto error_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              actual_wakeup - *active_wake_deadline_)
                              .count();
    if (adaptive_) {
        record_error(error_us);
    }
    timer_wakeup_pending_ = true;
    return VideoSyncWakeupObservation{
        .error_us = error_us,
        .compensation_us = compensation_us_,
    };
}

std::optional<std::uint64_t> VideoSyncWakeupController::wait_for_target(
    Clock::time_point presentation_deadline) noexcept {
    if (!active_presentation_deadline_ || !active_wake_deadline_ ||
        *active_presentation_deadline_ != presentation_deadline ||
        (!timer_wakeup_pending_ && Clock::now() < *active_wake_deadline_)) {
        return std::nullopt;
    }

    timer_wakeup_pending_ = false;
    const auto spin_start = presentation_deadline - spin_window_;
    if (const auto now = Clock::now(); now < spin_start) {
        std::this_thread::sleep_until(spin_start);
    }

    const auto busy_wait_started_at = Clock::now();
    while (Clock::now() < presentation_deadline) {
        spin_pause();
    }
    const auto busy_wait_duration = std::chrono::duration_cast<
        std::chrono::microseconds>(Clock::now() - busy_wait_started_at);
    return static_cast<std::uint64_t>(std::max<std::int64_t>(
        0, busy_wait_duration.count()));
}

std::int64_t VideoSyncWakeupController::compensation_us() const noexcept {
    return compensation_us_;
}

void VideoSyncWakeupController::record_error(std::int64_t error_us) noexcept {
    if (error_count_ < kErrorWindowSize) {
        error_window_[error_count_] = error_us;
        ++error_count_;
    } else {
        error_sum_us_ -= error_window_[error_index_];
        error_window_[error_index_] = error_us;
        error_index_ = (error_index_ + 1) % kErrorWindowSize;
    }
    error_sum_us_ += error_us;

    const auto average_error_us =
        error_sum_us_ / static_cast<std::int64_t>(error_count_);
    compensation_us_ = clamp_compensation(average_error_us);
}

std::int64_t VideoSyncWakeupController::clamp_compensation(
    std::int64_t compensation_us) const noexcept {
    const auto limit = max_compensation_.count();
    return std::clamp(compensation_us, -limit, limit);
}

void VideoSyncWakeupController::spin_pause() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

} // namespace semi::domain
