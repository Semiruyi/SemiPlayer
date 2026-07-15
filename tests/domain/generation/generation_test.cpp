#include "domain/generation/generation.hpp"

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

TEST(Generation, ConcurrentBumpsAreAtomic) {
    Generation g;
    constexpr int kThreads = 8;
    constexpr int kBumpsPerThread = 10000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&g] {
            for (int j = 0; j < kBumpsPerThread; ++j) g.bump();
        });
    }
    for (auto& t : threads) t.join();
    // 无锁 bump 必须无丢失：最终值 == 总 bump 次数。
    EXPECT_EQ(g.current(), static_cast<uint32_t>(kThreads * kBumpsPerThread));
}