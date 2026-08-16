#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace semi::domain {

struct VideoSyncWakeupOptions final {
    // Start the final timing phase this long before the presentation deadline.
    std::chrono::microseconds spin_window{500};
    // Bound the learned correction applied to the coarse wake deadline.
    std::chrono::microseconds max_compensation{16'000};
    bool adaptive = true;
};

struct VideoSyncWakeupObservation final {
    std::int64_t error_us = 0;
    std::int64_t compensation_us = 0;
};

// Converts a presentation deadline into an interruptible coarse wakeup and a
// short final timing phase. It learns the signed timer error from actual
// timeout wakeups; notifier-driven wakeups are not fed into the estimator.
class VideoSyncWakeupController final {
public:
    using Clock = std::chrono::steady_clock;

    explicit VideoSyncWakeupController(
        VideoSyncWakeupOptions options = {}) noexcept;

    VideoSyncWakeupController(const VideoSyncWakeupController&) = delete;
    VideoSyncWakeupController& operator=(const VideoSyncWakeupController&) = delete;
    VideoSyncWakeupController(VideoSyncWakeupController&&) = delete;
    VideoSyncWakeupController& operator=(VideoSyncWakeupController&&) = delete;

    void configure(VideoSyncWakeupOptions options) noexcept;
    void reset() noexcept;
    void clear_active_plan() noexcept;

    [[nodiscard]] Clock::time_point
    wake_deadline(Clock::time_point presentation_deadline) noexcept;

    [[nodiscard]] bool timer_wakeup_pending(
        Clock::time_point presentation_deadline) const noexcept;

    [[nodiscard]] std::optional<VideoSyncWakeupObservation>
    observe_timer_wakeup(Clock::time_point presentation_deadline,
                         Clock::time_point actual_wakeup) noexcept;

    // Called without the VideoSync mutex held. If the coarse wake deadline has
    // already arrived, it waits until the final spin window and then uses a
    // short active wait up to the presentation target. This also handles a
    // presentation deadline re-armed after an early scheduler pass.
    [[nodiscard]] std::optional<std::uint64_t>
    wait_for_target(Clock::time_point presentation_deadline) noexcept;

    [[nodiscard]] std::int64_t compensation_us() const noexcept;

private:
    static constexpr std::size_t kErrorWindowSize = 32;

    void record_error(std::int64_t error_us) noexcept;
    [[nodiscard]] std::int64_t clamp_compensation(
        std::int64_t compensation_us) const noexcept;
    static void spin_pause() noexcept;

    bool adaptive_ = true;
    std::chrono::microseconds spin_window_{1'000};
    std::chrono::microseconds max_compensation_{12'000};

    std::array<std::int64_t, kErrorWindowSize> error_window_{};
    std::size_t error_count_ = 0;
    std::size_t error_index_ = 0;
    std::int64_t error_sum_us_ = 0;
    std::int64_t compensation_us_ = 0;

    std::optional<Clock::time_point> active_presentation_deadline_;
    std::optional<Clock::time_point> active_wake_deadline_;
    bool timer_wakeup_pending_ = false;
};

} // namespace semi::domain
