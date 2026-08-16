#include "domain/worker/video_sync/video_sync_wakeup.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace semi::domain {
namespace {

using namespace std::chrono_literals;

TEST(VideoSyncWakeupControllerTest, LearnsSignedTimerBiasFromAWindowOfSamples) {
    VideoSyncWakeupController controller(VideoSyncWakeupOptions{
        .spin_window = 1ms,
        .max_compensation = 10ms,
        .adaptive = true,
    });

    const auto first_target = VideoSyncWakeupController::Clock::now() + 100ms;
    const auto first_wake = controller.wake_deadline(first_target);
    const auto first_observation = controller.observe_timer_wakeup(
        first_target, first_wake + 4ms);

    ASSERT_TRUE(first_observation);
    EXPECT_EQ(first_observation->error_us, 4'000);
    EXPECT_EQ(first_observation->compensation_us, 4'000);

    const auto second_target = first_target + 100ms;
    const auto second_wake = controller.wake_deadline(second_target);
    EXPECT_EQ(second_wake, second_target - 5ms);

    const auto second_observation = controller.observe_timer_wakeup(
        second_target, second_wake - 2ms);
    ASSERT_TRUE(second_observation);
    EXPECT_EQ(second_observation->error_us, -2'000);
    EXPECT_EQ(second_observation->compensation_us, 1'000);

    const auto third_target = second_target + 100ms;
    const auto third_wake = controller.wake_deadline(third_target);
    const auto third_observation = controller.observe_timer_wakeup(
        third_target, third_wake + 2ms);

    ASSERT_TRUE(third_observation);
    EXPECT_EQ(third_observation->error_us, 2'000);
    EXPECT_EQ(third_observation->compensation_us, 1'333);
}

TEST(VideoSyncWakeupControllerTest, KeepsThePresentationDeadlineSeparateFromWakeDeadline) {
    VideoSyncWakeupController controller(VideoSyncWakeupOptions{
        .spin_window = 1ms,
        .max_compensation = 10ms,
        .adaptive = true,
    });
    const auto target = VideoSyncWakeupController::Clock::now() + 100ms;

    const auto wake = controller.wake_deadline(target);
    EXPECT_EQ(wake, target - 1ms);

    const auto observation = controller.observe_timer_wakeup(target, wake + 6ms);
    ASSERT_TRUE(observation);
    EXPECT_EQ(controller.compensation_us(), 6'000);

    const auto same_target_wake = controller.wake_deadline(target);
    EXPECT_EQ(same_target_wake, wake);

    controller.clear_active_plan();
    const auto next_target = target + 100ms;
    EXPECT_EQ(controller.wake_deadline(next_target), next_target - 7ms);
}

TEST(VideoSyncWakeupControllerTest, DisablesAdaptationWithoutChangingTheSpinWindow) {
    VideoSyncWakeupController controller(VideoSyncWakeupOptions{
        .spin_window = 2ms,
        .max_compensation = 10ms,
        .adaptive = false,
    });
    const auto target = VideoSyncWakeupController::Clock::now() + 100ms;
    const auto wake = controller.wake_deadline(target);

    const auto observation = controller.observe_timer_wakeup(target, wake + 8ms);

    ASSERT_TRUE(observation);
    EXPECT_EQ(observation->compensation_us, 0);
    EXPECT_EQ(controller.compensation_us(), 0);
}

TEST(VideoSyncWakeupControllerTest, CapsTheLearnedCompensation) {
    VideoSyncWakeupController controller(VideoSyncWakeupOptions{
        .spin_window = 1ms,
        .max_compensation = 3ms,
        .adaptive = true,
    });
    const auto target = VideoSyncWakeupController::Clock::now() + 100ms;
    const auto wake = controller.wake_deadline(target);

    const auto observation = controller.observe_timer_wakeup(target, wake + 10ms);

    ASSERT_TRUE(observation);
    EXPECT_EQ(observation->compensation_us, 3'000);
    EXPECT_EQ(controller.compensation_us(), 3'000);
}

} // namespace
} // namespace semi::domain
