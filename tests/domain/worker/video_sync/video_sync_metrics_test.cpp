#include "domain/worker/video_sync/video_sync_metrics.hpp"

#include <gtest/gtest.h>

namespace semi::domain {
namespace {

TEST(VideoSyncMetricsTest, AccumulatesPresentationAndSchedulingFacts) {
    VideoSyncMetrics metrics;
    metrics.on_session_started(7);
    metrics.on_playback_started();
    metrics.on_frame_popped();
    metrics.on_frame_popped();
    metrics.on_frame_dropped_for_catchup();
    metrics.on_stale_item_dropped();
    metrics.on_empty_pop();
    metrics.on_audio_clock_unavailable();
    metrics.on_wait_scheduled(1'000);
    metrics.on_wait_scheduled(3'000);
    metrics.on_wait_overshoot(40);
    metrics.on_frame_presented(VideoSyncPresentationObservation{
        .frame_pts_us = 100,
        .clock_pts_us = 250,
        .callback_duration_us = 12,
    });

    const auto snapshot = metrics.snapshot();

    EXPECT_EQ(snapshot.generation, 7U);
    EXPECT_EQ(snapshot.rendered_frames_popped, 2U);
    EXPECT_EQ(snapshot.frames_presented, 1U);
    EXPECT_EQ(snapshot.frames_dropped_for_catchup, 1U);
    EXPECT_EQ(snapshot.stale_items_dropped, 1U);
    EXPECT_EQ(snapshot.empty_pop_attempts, 1U);
    EXPECT_EQ(snapshot.audio_clock_unavailable, 1U);
    EXPECT_EQ(snapshot.wait_events, 2U);
    EXPECT_EQ(snapshot.wait_target_total_us, 4'000U);
    EXPECT_EQ(snapshot.max_wait_target_us, 3'000U);
    EXPECT_DOUBLE_EQ(snapshot.wait_target_average_us, 2'000.0);
    EXPECT_EQ(snapshot.wait_overshoot_events, 1U);
    EXPECT_EQ(snapshot.wait_overshoot_total_us, 40U);
    EXPECT_EQ(snapshot.max_wait_overshoot_us, 40U);
    EXPECT_DOUBLE_EQ(snapshot.wait_overshoot_average_us, 40.0);
    EXPECT_EQ(snapshot.presented_late_frames, 1U);
    EXPECT_EQ(snapshot.presented_lateness_total_us, 150U);
    EXPECT_EQ(snapshot.max_presented_lateness_us, 150U);
    EXPECT_DOUBLE_EQ(snapshot.presented_lateness_average_us, 150.0);
    EXPECT_EQ(snapshot.callback_duration_total_us, 12U);
    EXPECT_EQ(snapshot.max_callback_duration_us, 12U);
    EXPECT_DOUBLE_EQ(snapshot.callback_duration_average_us, 12.0);
    EXPECT_GE(snapshot.elapsed_ms, 0.0);
}

TEST(VideoSyncMetricsTest, StartingANewSessionResetsPreviousFacts) {
    VideoSyncMetrics metrics;
    metrics.on_session_started(3);
    metrics.on_frame_popped();
    metrics.on_session_started(4);

    const auto snapshot = metrics.snapshot();

    EXPECT_EQ(snapshot.generation, 4U);
    EXPECT_EQ(snapshot.rendered_frames_popped, 0U);
    EXPECT_EQ(snapshot.frames_presented, 0U);
    EXPECT_DOUBLE_EQ(snapshot.elapsed_ms, 0.0);
    EXPECT_DOUBLE_EQ(snapshot.fps, 0.0);
}

} // namespace
} // namespace semi::domain
