#include "domain/resource/generation/generation.hpp"
#include "domain/resource/generation/generation_events.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using semi::domain::Generation;

TEST(Generation, StartsAtZero) {
    Generation g;
    EXPECT_EQ(g.current(), 0u);
    EXPECT_TRUE(g.is_current(0));
}

TEST(Generation, BumpIncrements) {
    Generation g;
    g.bump();
    EXPECT_EQ(g.current(), 1u);
    EXPECT_TRUE(g.is_current(1));
    EXPECT_FALSE(g.is_current(0));
    g.bump();
    EXPECT_EQ(g.current(), 2u);
}

TEST(Generation, AssociatesAccurateSeekTargetWithOnlyItsGeneration) {
    Generation generation;

    const auto accurate = generation.bump(1'234'567);
    const auto snapshot = generation.snapshot();
    EXPECT_EQ(snapshot.value, accurate);
    EXPECT_EQ(snapshot.seek_target_pts_us, 1'234'567);
    EXPECT_EQ(generation.seek_target_for(accurate), 1'234'567);
    EXPECT_FALSE(generation.seek_target_for(accurate - 1).has_value());

    const auto ordinary = generation.bump();
    EXPECT_EQ(ordinary, accurate + 1);
    EXPECT_FALSE(generation.seek_target_for(ordinary).has_value());
    EXPECT_FALSE(generation.seek_target_for(accurate).has_value());
}

TEST(Generation, BumpPublishesTheNewValue) {
    auto notifier = std::make_shared<semi::infra::DefaultNotifier>();
    Generation generation(notifier);
    std::vector<Generation::Value> observed;
    const auto subscription = notifier->subscribe<semi::domain::GenerationChanged>(
        [&observed](const semi::domain::GenerationChanged& event) {
            observed.push_back(event.value);
        });

    EXPECT_EQ(generation.bump(), 1u);
    EXPECT_EQ(generation.bump(), 2u);
    EXPECT_EQ(observed, (std::vector<Generation::Value>{1u, 2u}));
    EXPECT_TRUE(subscription->active());
}

TEST(Generation, ConcurrentBumpsAreAtomic) {
    Generation g;
    constexpr int kThreads = 8;
    constexpr int kBumpsPerThread = 10000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&g] {
            for (int j = 0; j < kBumpsPerThread; ++j)
                g.bump();
        });
    }
    for (auto& t : threads)
        t.join();
    // 并发 bump 必须无丢失：最终值 == 总 bump 次数。
    EXPECT_EQ(g.current(), static_cast<uint32_t>(kThreads * kBumpsPerThread));
}
