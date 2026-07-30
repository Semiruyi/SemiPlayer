#include "domain/resource/audio_frame_store/audio_frame_store.hpp"
#include "domain/resource/audio_packet_queue/audio_packet_queue.hpp"
#include "domain/worker/audio_decoder/default_audio_decoder.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace semi::domain {
namespace {

using contracts::audio_decoder::DecodedAudioBatch;

class FakeAudioDecoderBackend final : public AudioDecoderBackend {
public:
    std::expected<void, AudioDecoderBackendError>
    configure(const contracts::media::AudioCodecConfig&) override {
        ++configure_calls;
        if (fail_configure) {
            return std::unexpected(AudioDecoderBackendError{
                .operation = AudioDecoderBackendOperation::Configure,
                .native_code = -1,
                .message = "fake configure failure",
            });
        }
        return {};
    }

    std::expected<DecodedAudioBatch, AudioDecoderBackendError>
    decode(const contracts::demuxer::packet::EncodedPacket& packet) override {
        ++decode_calls;
        if (fail_decode) {
            return std::unexpected(AudioDecoderBackendError{
                .operation = AudioDecoderBackendOperation::Decode,
                .native_code = -2,
                .message = "fake decode failure",
            });
        }

        DecodedAudioBatch result;
        for (int index = 0; index < frames_per_decode; ++index) {
            result.push_back(make_audio(packet.pts_us.value_or(0) + index));
        }
        return result;
    }

    std::expected<DecodedAudioBatch, AudioDecoderBackendError> drain() override {
        ++drain_calls;
        return drain_frames;
    }

    void reset() noexcept override {
        ++reset_calls;
    }

    void unconfigure() noexcept override {
        ++unconfigure_calls;
    }

    static contracts::media::DecodedAudio make_audio(std::int64_t pts_us) {
        return contracts::media::DecodedAudio{
            .format = contracts::media::AudioPcmFormat{
                .sample_rate = 48'000,
                .channels = 2,
                .sample_format = contracts::media::AudioSampleFormat::F32,
                .planar = false,
            },
            .samples_per_channel = 1,
            .planes = {{std::byte{0x01}}},
            .pts_us = pts_us,
        };
    }

    std::atomic_int configure_calls{0};
    std::atomic_int decode_calls{0};
    std::atomic_int drain_calls{0};
    std::atomic_int reset_calls{0};
    std::atomic_int unconfigure_calls{0};
    std::atomic_bool fail_configure{false};
    std::atomic_bool fail_decode{false};
    int frames_per_decode = 1;
    DecodedAudioBatch drain_frames;
};

bool wait_until(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return predicate();
}

AudioPacket make_packet(std::int64_t pts_us, Generation::Value generation) {
    return AudioPacket(
        contracts::demuxer::packet::EncodedPacket{
            .payload = {std::byte{0x01}},
            .pts_us = pts_us,
            .dts_us = pts_us,
            .duration_us = 1'000,
        },
        generation);
}

contracts::media::AudioCodecConfig audio_config() {
    return contracts::media::AudioCodecConfig{
        .common = {.codec_name = "fake", .extradata = {}},
        .sample_rate = 48'000,
        .channels = 2,
    };
}

struct DecoderDependencies {
    std::shared_ptr<infra::DefaultNotifier> notifier = std::make_shared<infra::DefaultNotifier>();
    std::shared_ptr<AudioPacketQueue> packet_queue = std::make_shared<AudioPacketQueue>(notifier);
    std::shared_ptr<AudioFrameStore> frame_store = std::make_shared<AudioFrameStore>(notifier);
    std::shared_ptr<FakeAudioDecoderBackend> backend =
        std::make_shared<FakeAudioDecoderBackend>();
    std::shared_ptr<Generation> generation = std::make_shared<Generation>();
};

std::unique_ptr<DefaultAudioDecoder> make_decoder(const DecoderDependencies& dependencies) {
    return std::make_unique<DefaultAudioDecoder>(
        dependencies.packet_queue,
        dependencies.frame_store,
        dependencies.backend,
        dependencies.notifier,
        dependencies.generation);
}

const AudioFrame* frame_value(const AudioFrameStoreItem& item) noexcept {
    return std::get_if<AudioFrame>(&item);
}

TEST(DefaultAudioDecoderTest, DecodesPacketsAndEmitsOrderedEndOfInput) {
    DecoderDependencies dependencies;
    dependencies.backend->drain_frames.push_back(FakeAudioDecoderBackend::make_audio(20));
    auto decoder = make_decoder(dependencies);

    ASSERT_TRUE(decoder->configure(audio_config()).has_value());
    ASSERT_TRUE(decoder->start().has_value());
    ASSERT_EQ(dependencies.packet_queue->try_push(
                  AudioPacketQueueItem{make_packet(10, dependencies.generation->current())}),
              AudioPacketPushResult::Accepted);
    ASSERT_EQ(dependencies.packet_queue->try_push(AudioPacketQueueItem{
                  AudioPacketEndOfInput{.generation = dependencies.generation->current()}}),
              AudioPacketPushResult::Accepted);

    ASSERT_TRUE(wait_until([&dependencies] {
        return dependencies.frame_store->size() == 3;
    }));

    auto first = dependencies.frame_store->try_pop();
    ASSERT_TRUE(first.has_value());
    const auto* first_frame = frame_value(*first);
    ASSERT_NE(first_frame, nullptr);
    ASSERT_TRUE(first_frame->decoded().pts_us.has_value());
    EXPECT_EQ(*first_frame->decoded().pts_us, 10);

    auto drained = dependencies.frame_store->try_pop();
    ASSERT_TRUE(drained.has_value());
    const auto* drained_frame = frame_value(*drained);
    ASSERT_NE(drained_frame, nullptr);
    ASSERT_TRUE(drained_frame->decoded().pts_us.has_value());
    EXPECT_EQ(*drained_frame->decoded().pts_us, 20);

    auto end = dependencies.frame_store->try_pop();
    ASSERT_TRUE(end.has_value());
    const auto* end_of_input = std::get_if<AudioFrameEndOfInput>(&*end);
    ASSERT_NE(end_of_input, nullptr);
    EXPECT_EQ(end_of_input->generation, dependencies.generation->current());
    EXPECT_EQ(dependencies.backend->decode_calls.load(), 1);
    EXPECT_EQ(dependencies.backend->drain_calls.load(), 1);

    decoder->stop();
    decoder->unconfigure();
}

TEST(DefaultAudioDecoderTest, StopsWhileWaitingForInput) {
    DecoderDependencies dependencies;
    auto decoder = make_decoder(dependencies);

    ASSERT_TRUE(decoder->configure(audio_config()).has_value());
    ASSERT_TRUE(decoder->start().has_value());

    const auto begin = std::chrono::steady_clock::now();
    decoder->stop();
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    EXPECT_LT(elapsed, std::chrono::seconds{1});
    decoder->unconfigure();
}

TEST(DefaultAudioDecoderTest, PreservesPendingOutputOrderUnderBackpressure) {
    DecoderDependencies dependencies;
    dependencies.frame_store = std::make_shared<AudioFrameStore>(dependencies.notifier, 1);
    dependencies.backend->frames_per_decode = 2;
    auto decoder = make_decoder(dependencies);

    ASSERT_TRUE(decoder->configure(audio_config()).has_value());
    ASSERT_TRUE(decoder->start().has_value());
    ASSERT_EQ(dependencies.packet_queue->try_push(
                  AudioPacketQueueItem{make_packet(10, dependencies.generation->current())}),
              AudioPacketPushResult::Accepted);

    ASSERT_TRUE(wait_until([&dependencies] {
        return dependencies.backend->decode_calls.load() == 1 &&
               dependencies.frame_store->full();
    }));

    auto first = dependencies.frame_store->try_pop();
    ASSERT_TRUE(first.has_value());
    const auto* first_frame = frame_value(*first);
    ASSERT_NE(first_frame, nullptr);
    ASSERT_TRUE(first_frame->decoded().pts_us.has_value());
    EXPECT_EQ(*first_frame->decoded().pts_us, 10);

    ASSERT_TRUE(wait_until([&dependencies] {
        return !dependencies.frame_store->empty();
    }));
    auto second = dependencies.frame_store->try_pop();
    ASSERT_TRUE(second.has_value());
    const auto* second_frame = frame_value(*second);
    ASSERT_NE(second_frame, nullptr);
    ASSERT_TRUE(second_frame->decoded().pts_us.has_value());
    EXPECT_EQ(*second_frame->decoded().pts_us, 11);

    decoder->stop();
    decoder->unconfigure();
}

TEST(DefaultAudioDecoderTest, ResetsBackendAndDropsOldPendingOutputOnGenerationChange) {
    DecoderDependencies dependencies;
    dependencies.frame_store = std::make_shared<AudioFrameStore>(dependencies.notifier, 1);
    dependencies.backend->frames_per_decode = 2;
    auto decoder = make_decoder(dependencies);

    ASSERT_TRUE(decoder->configure(audio_config()).has_value());
    ASSERT_TRUE(decoder->start().has_value());
    ASSERT_EQ(dependencies.packet_queue->try_push(
                  AudioPacketQueueItem{make_packet(10, dependencies.generation->current())}),
              AudioPacketPushResult::Accepted);
    ASSERT_TRUE(wait_until([&dependencies] {
        return dependencies.backend->decode_calls.load() == 1 &&
               dependencies.frame_store->full();
    }));

    dependencies.generation->bump();
    ASSERT_EQ(dependencies.packet_queue->try_push(
                  AudioPacketQueueItem{make_packet(20, dependencies.generation->current())}),
              AudioPacketPushResult::Accepted);

    auto old_frame = dependencies.frame_store->try_pop();
    ASSERT_TRUE(old_frame.has_value());
    ASSERT_NE(frame_value(*old_frame), nullptr);

    ASSERT_TRUE(wait_until([&dependencies] {
        return dependencies.backend->decode_calls.load() == 2 &&
               dependencies.backend->reset_calls.load() == 1 &&
               !dependencies.frame_store->empty();
    }));
    auto new_frame = dependencies.frame_store->try_pop();
    ASSERT_TRUE(new_frame.has_value());
    const auto* frame = frame_value(*new_frame);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->generation(), dependencies.generation->current());
    ASSERT_TRUE(frame->decoded().pts_us.has_value());
    EXPECT_EQ(*frame->decoded().pts_us, 20);

    decoder->stop();
    decoder->unconfigure();
}

TEST(DefaultAudioDecoderTest, IgnoresPacketsAfterEndOfInputInTheSameGeneration) {
    DecoderDependencies dependencies;
    auto decoder = make_decoder(dependencies);
    const auto generation = dependencies.generation->current();

    ASSERT_TRUE(decoder->configure(audio_config()).has_value());
    ASSERT_TRUE(decoder->start().has_value());
    ASSERT_EQ(dependencies.packet_queue->try_push(AudioPacketQueueItem{make_packet(10, generation)}),
              AudioPacketPushResult::Accepted);
    ASSERT_EQ(dependencies.packet_queue->try_push(
                  AudioPacketQueueItem{AudioPacketEndOfInput{.generation = generation}}),
              AudioPacketPushResult::Accepted);
    ASSERT_EQ(dependencies.packet_queue->try_push(AudioPacketQueueItem{make_packet(30, generation)}),
              AudioPacketPushResult::Accepted);

    ASSERT_TRUE(wait_until([&dependencies] {
        return dependencies.backend->decode_calls.load() == 1 &&
               dependencies.backend->drain_calls.load() == 1 &&
               dependencies.packet_queue->empty() &&
               dependencies.frame_store->size() == 2;
    }));

    decoder->stop();
    decoder->unconfigure();
}

TEST(DefaultAudioDecoderTest, PublishesBackendFailureAndRequiresUnconfigure) {
    DecoderDependencies dependencies;
    dependencies.backend->fail_decode = true;
    std::atomic_int failure_notifications{0};
    auto failure_subscription = dependencies.notifier->subscribe<AudioDecoderBackendFailure>(
        [&failure_notifications](const AudioDecoderBackendFailure&) {
            ++failure_notifications;
        });
    auto decoder = make_decoder(dependencies);

    ASSERT_TRUE(decoder->configure(audio_config()).has_value());
    ASSERT_TRUE(decoder->start().has_value());
    ASSERT_EQ(dependencies.packet_queue->try_push(
                  AudioPacketQueueItem{make_packet(10, dependencies.generation->current())}),
              AudioPacketPushResult::Accepted);

    ASSERT_TRUE(wait_until([&failure_notifications] {
        return failure_notifications.load() == 1;
    }));
    decoder->stop();

    const auto restart = decoder->start();
    ASSERT_FALSE(restart.has_value());
    EXPECT_EQ(restart.error().code, AudioDecoderErrorCode::InvalidState);

    decoder->unconfigure();
    EXPECT_EQ(dependencies.backend->unconfigure_calls.load(), 1);
    EXPECT_TRUE(failure_subscription->active());
}

} // namespace
} // namespace semi::domain
