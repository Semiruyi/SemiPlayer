#include "infrastructure/notifier/realtime_notifier.hpp"

#include <gtest/gtest.h>

namespace semi::infra {
namespace {

struct FramesConsumed {
    int frames = 0;
};

struct DeviceUnderrun {
    int count = 0;
};

class FrameSink final : public RealTimeNotificationSink<FramesConsumed> {
public:
    void on_realtime_notification(const FramesConsumed& event) noexcept override {
        calls += 1;
        total_frames += event.frames;
    }

    int calls = 0;
    int total_frames = 0;
};

class UnderrunSink final : public RealTimeNotificationSink<DeviceUnderrun> {
public:
    void on_realtime_notification(const DeviceUnderrun& event) noexcept override {
        calls += 1;
        total_underruns += event.count;
    }

    int calls = 0;
    int total_underruns = 0;
};

using TestNotifier = RealTimeNotifier<
    RealTimeEventSpec<FramesConsumed, 2>,
    RealTimeEventSpec<DeviceUnderrun, 1>>;

TEST(RealTimeNotifierTest, RoutesOnlyToSinksRegisteredForTheEventType) {
    TestNotifier notifier;
    FrameSink frame_sink;
    UnderrunSink underrun_sink;

    ASSERT_TRUE(notifier.register_sink(frame_sink));
    ASSERT_TRUE(notifier.register_sink(underrun_sink));
    ASSERT_TRUE(notifier.seal());

    notifier.notify(FramesConsumed{.frames = 480});

    EXPECT_EQ(frame_sink.calls, 1);
    EXPECT_EQ(frame_sink.total_frames, 480);
    EXPECT_EQ(underrun_sink.calls, 0);
}

TEST(RealTimeNotifierTest, DeliversToEverySinkInTheFixedRoute) {
    TestNotifier notifier;
    FrameSink first;
    FrameSink second;

    ASSERT_TRUE(notifier.register_sink(first));
    ASSERT_TRUE(notifier.register_sink(second));
    ASSERT_TRUE(notifier.seal());

    notifier.notify(FramesConsumed{.frames = 96});

    EXPECT_EQ(first.total_frames, 96);
    EXPECT_EQ(second.total_frames, 96);
}

TEST(RealTimeNotifierTest, RejectsDuplicateAndOverCapacityRegistrations) {
    TestNotifier notifier;
    FrameSink first;
    FrameSink second;
    FrameSink third;

    EXPECT_TRUE(notifier.register_sink(first));
    EXPECT_FALSE(notifier.register_sink(first));
    EXPECT_TRUE(notifier.register_sink(second));
    EXPECT_FALSE(notifier.register_sink(third));
}

TEST(RealTimeNotifierTest, AllowsTopologyChangesOnlyWhileUnsealed) {
    TestNotifier notifier;
    FrameSink frame_sink;
    UnderrunSink underrun_sink;

    ASSERT_TRUE(notifier.register_sink(frame_sink));
    ASSERT_TRUE(notifier.seal());
    EXPECT_TRUE(notifier.sealed());

#ifdef NDEBUG
    EXPECT_FALSE(notifier.register_sink(underrun_sink));
    EXPECT_FALSE(notifier.unregister_sink(frame_sink));
#endif

    ASSERT_TRUE(notifier.unseal());
    EXPECT_FALSE(notifier.sealed());
    EXPECT_TRUE(notifier.unregister_sink(frame_sink));
    EXPECT_TRUE(notifier.register_sink(underrun_sink));
}

} // namespace
} // namespace semi::infra
