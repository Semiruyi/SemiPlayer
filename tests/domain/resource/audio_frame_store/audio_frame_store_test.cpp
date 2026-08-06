#include "domain/resource/audio_frame_store/audio_frame_store.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include <gtest/gtest.h>

namespace semi::domain {
namespace {

AudioFrame make_frame(std::uint8_t marker, Generation::Value generation) {
    contracts::media::DecodedAudio decoded;
    decoded.format.sample_rate = 48'000;
    decoded.format.channels = 1;
    decoded.format.sample_format = contracts::media::AudioSampleFormat::S16;
    decoded.format.planar = false;
    decoded.samples_per_channel = 1;
    decoded.planes.push_back({std::byte{marker}, std::byte{0}});
    decoded.pts_us = marker;
    return AudioFrame(std::move(decoded), generation);
}

std::uint8_t frame_marker(const AudioFrame& frame) {
    return std::to_integer<std::uint8_t>(frame.decoded().planes.front().front());
}

TEST(AudioFrameStore, PreservesFifoOrderAndGeneration) {
    AudioFrameStore store(std::make_shared<infra::DefaultNotifier>(), 2);

    EXPECT_EQ(store.try_push(make_frame(1, 10)), AudioFramePushResult::Accepted);
    EXPECT_EQ(store.try_push(make_frame(2, 11)), AudioFramePushResult::Accepted);
    EXPECT_TRUE(store.full());

    auto first = store.try_pop();
    ASSERT_TRUE(first.has_value());
    const auto* first_frame = std::get_if<AudioFrame>(&*first);
    ASSERT_NE(first_frame, nullptr);
    EXPECT_EQ(frame_marker(*first_frame), 1U);
    EXPECT_EQ(first_frame->generation(), 10U);

    auto second = store.try_pop();
    ASSERT_TRUE(second.has_value());
    const auto* second_frame = std::get_if<AudioFrame>(&*second);
    ASSERT_NE(second_frame, nullptr);
    EXPECT_EQ(frame_marker(*second_frame), 2U);
    EXPECT_EQ(second_frame->generation(), 11U);
    EXPECT_TRUE(store.empty());
}

TEST(AudioFrameStore, PreservesEndOfInputAfterPcm) {
    AudioFrameStore store(std::make_shared<infra::DefaultNotifier>(), 2);
    ASSERT_EQ(store.try_push(make_frame(1, 10)), AudioFramePushResult::Accepted);
    ASSERT_EQ(store.try_push(AudioFrameEndOfInput{.generation = 10}),
              AudioFramePushResult::Accepted);

    auto frame = store.try_pop();
    ASSERT_TRUE(frame.has_value());
    EXPECT_NE(std::get_if<AudioFrame>(&*frame), nullptr);

    auto end_of_input = store.try_pop();
    ASSERT_TRUE(end_of_input.has_value());
    const auto* marker = std::get_if<AudioFrameEndOfInput>(&*end_of_input);
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->generation, 10U);
}

TEST(AudioFrame, ChecksGeneration) {
    const AudioFrame frame = make_frame(1, 7);

    EXPECT_TRUE(is_current_audio_frame(frame, 7));
    EXPECT_FALSE(is_current_audio_frame(frame, 8));
}

TEST(AudioFrameStore, FullPushDoesNotConsumeFrame) {
    AudioFrameStore store(std::make_shared<infra::DefaultNotifier>(), 1);
    ASSERT_EQ(store.try_push(make_frame(1, 10)), AudioFramePushResult::Accepted);

    AudioFrameStoreItem rejected = make_frame(2, 11);
    EXPECT_EQ(store.try_push(std::move(rejected)), AudioFramePushResult::Full);
    const auto* rejected_frame = std::get_if<AudioFrame>(&rejected);
    ASSERT_NE(rejected_frame, nullptr);
    EXPECT_EQ(frame_marker(*rejected_frame), 2U);
    EXPECT_EQ(rejected_frame->generation(), 11U);
}

TEST(AudioFrameStore, ZeroCapacityStoreAlwaysReportsFull) {
    AudioFrameStore store(std::make_shared<infra::DefaultNotifier>(), 0);
    AudioFrameStoreItem frame = make_frame(1, 10);

    EXPECT_EQ(store.try_push(std::move(frame)), AudioFramePushResult::Full);
    const auto* rejected_frame = std::get_if<AudioFrame>(&frame);
    ASSERT_NE(rejected_frame, nullptr);
    EXPECT_EQ(frame_marker(*rejected_frame), 1U);
    EXPECT_TRUE(store.full());
}

TEST(AudioFrameStore, NotifiesConsumerOnEmptyBoundaryAndProducerOnFullBoundary) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    AudioFrameStore store(notifier, 2);
    int not_empty_calls = 0;
    int not_full_calls = 0;
    auto not_empty_subscription = notifier->subscribe<AudioFrameStoreNotEmpty>(
        [&not_empty_calls, &store](const AudioFrameStoreNotEmpty&) {
            ++not_empty_calls;
            EXPECT_FALSE(store.empty());
        });
    auto not_full_subscription = notifier->subscribe<AudioFrameStoreNotFull>(
        [&not_full_calls, &store](const AudioFrameStoreNotFull&) {
            ++not_full_calls;
            EXPECT_FALSE(store.full());
        });

    EXPECT_EQ(store.try_push(make_frame(1, 10)), AudioFramePushResult::Accepted);
    EXPECT_EQ(store.try_push(make_frame(2, 11)), AudioFramePushResult::Accepted);
    EXPECT_EQ(not_empty_calls, 1);
    EXPECT_EQ(not_full_calls, 0);

    ASSERT_TRUE(store.try_pop().has_value());
    EXPECT_EQ(not_full_calls, 1);
    ASSERT_TRUE(store.try_pop().has_value());
    EXPECT_EQ(not_full_calls, 1);

    EXPECT_EQ(store.try_push(make_frame(3, 12)), AudioFramePushResult::Accepted);
    EXPECT_EQ(not_empty_calls, 2);
    EXPECT_EQ(not_full_calls, 1);

    EXPECT_TRUE(not_empty_subscription->active());
    EXPECT_TRUE(not_full_subscription->active());
}

TEST(AudioFrameStore, ExposesIndependentProducerAndConsumerPorts) {
    AudioFrameStore store(std::make_shared<infra::DefaultNotifier>(), 1);
    AudioFrameSink& sink = store;
    AudioFrameSource& source = store;

    EXPECT_EQ(sink.try_push(make_frame(1, 10)), AudioFramePushResult::Accepted);
    auto frame = source.try_pop();
    ASSERT_TRUE(frame.has_value());
    const auto* audio_frame = std::get_if<AudioFrame>(&*frame);
    ASSERT_NE(audio_frame, nullptr);
    EXPECT_EQ(frame_marker(*audio_frame), 1U);
}

} // namespace
} // namespace semi::domain
