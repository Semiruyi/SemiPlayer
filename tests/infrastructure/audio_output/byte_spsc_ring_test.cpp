#include "infrastructure/audio_output/byte_spsc_ring.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace semi::infra::audio_output {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

TEST(ByteSpscRingTest, StartsEmptyAndReadsNothing) {
    ByteSpscRing ring;
    ring.resize(8);
    std::array<std::byte, 4> output{};

    EXPECT_EQ(ring.available(), 0U);
    EXPECT_EQ(ring.try_read(output.data(), output.size()), 0U);
}

TEST(ByteSpscRingTest, PreservesByteOrder) {
    ByteSpscRing ring;
    ring.resize(8);
    const auto input = bytes({1, 2, 3, 4, 5});
    std::array<std::byte, 5> output{};

    ASSERT_TRUE(ring.try_write(input.data(), input.size()));
    ASSERT_EQ(ring.try_read(output.data(), output.size()), output.size());
    EXPECT_EQ(std::vector<std::byte>(output.begin(), output.end()), input);
    EXPECT_EQ(ring.available(), 0U);
}

TEST(ByteSpscRingTest, RejectsFullWriteWithoutPartialMutation) {
    ByteSpscRing ring;
    ring.resize(4);
    const auto first = bytes({1, 2, 3});
    const auto rejected = bytes({4, 5});
    std::array<std::byte, 4> output{};

    ASSERT_TRUE(ring.try_write(first.data(), first.size()));
    EXPECT_FALSE(ring.try_write(rejected.data(), rejected.size()));
    ASSERT_EQ(ring.try_read(output.data(), output.size()), first.size());
    EXPECT_EQ(std::vector<std::byte>(output.begin(), output.begin() + 3), first);
}

TEST(ByteSpscRingTest, WrapsWithoutReorderingBytes) {
    ByteSpscRing ring;
    ring.resize(5);
    const auto first = bytes({1, 2, 3, 4});
    const auto second = bytes({5, 6, 7});
    std::array<std::byte, 2> discarded{};
    std::array<std::byte, 5> output{};

    ASSERT_TRUE(ring.try_write(first.data(), first.size()));
    ASSERT_EQ(ring.try_read(discarded.data(), discarded.size()), discarded.size());
    ASSERT_TRUE(ring.try_write(second.data(), second.size()));
    ASSERT_EQ(ring.try_read(output.data(), output.size()), output.size());
    EXPECT_EQ(std::vector<std::byte>(output.begin(), output.end()), bytes({3, 4, 5, 6, 7}));
}

TEST(ByteSpscRingTest, ResetDropsBufferedDataAndKeepsTheRingReusable) {
    ByteSpscRing ring;
    ring.resize(4);
    const auto discarded = bytes({1, 2, 3});
    const auto retained = bytes({4, 5});
    std::array<std::byte, 2> output{};

    ASSERT_TRUE(ring.try_write(discarded.data(), discarded.size()));
    ring.reset();
    EXPECT_EQ(ring.available(), 0U);
    ASSERT_TRUE(ring.try_write(retained.data(), retained.size()));
    ASSERT_EQ(ring.try_read(output.data(), output.size()), output.size());
    EXPECT_EQ(std::vector<std::byte>(output.begin(), output.end()), retained);
}

TEST(ByteSpscRingTest, TransfersBytesBetweenOneProducerAndOneConsumer) {
    constexpr std::size_t kCapacity = 257;
    constexpr std::size_t kTotalBytes = 1U << 20U;
    constexpr std::size_t kWriteChunk = 31;
    constexpr std::size_t kReadChunk = 47;

    ByteSpscRing ring;
    ring.resize(kCapacity);
    std::atomic_bool producer_done = false;

    std::thread producer([&] {
        std::array<std::byte, kWriteChunk> input{};
        std::size_t produced = 0;
        while (produced < kTotalBytes) {
            const auto count = std::min(kWriteChunk, kTotalBytes - produced);
            for (std::size_t index = 0; index < count; ++index) {
                input[index] = static_cast<std::byte>((produced + index) % 251U);
            }
            if (ring.try_write(input.data(), count)) {
                produced += count;
            } else {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::array<std::byte, kReadChunk> output{};
    std::size_t consumed = 0;
    while (consumed < kTotalBytes) {
        const auto copied = ring.try_read(output.data(), output.size());
        if (copied == 0) {
            if (producer_done.load(std::memory_order_acquire)) {
                continue;
            }
            std::this_thread::yield();
            continue;
        }
        for (std::size_t index = 0; index < copied; ++index) {
            EXPECT_EQ(output[index], static_cast<std::byte>((consumed + index) % 251U));
        }
        consumed += copied;
    }
    producer.join();
    EXPECT_EQ(ring.available(), 0U);
}

} // namespace
} // namespace semi::infra::audio_output
