#include "domain/worker/audio_decoder/default_audio_decoder.hpp"

#include "contracts/audio_decoder/audio_decoder_backend.hpp"
#include "domain/resource/audio_frame_store/audio_frame_sink.hpp"
#include "domain/resource/audio_frame_store/audio_frame_store_item.hpp"
#include "domain/resource/audio_packet_queue/audio_packet.hpp"
#include "domain/resource/audio_packet_queue/audio_packet_source.hpp"
#include "domain/resource/audio_packet_queue/audio_packet_queue_events.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_decoder/audio_decoder_events.hpp"
#include "domain/worker/audio_decoder/audio_decoder.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace semi::domain {
namespace {

using contracts::audio_decoder::AudioDecoderBackend;
using contracts::audio_decoder::AudioDecoderBackendError;
using contracts::audio_decoder::AudioDecoderBackendOperation;
using contracts::audio_decoder::DecodedAudioBatch;
using contracts::demuxer::packet::EncodedPacket;

class FakeAudioDecoderBackend final : public AudioDecoderBackend {
public:
    std::expected<void, AudioDecoderBackendError>
    configure(const contracts::media::AudioCodecConfig&) override {
        ++configure_calls;
        std::lock_guard lock(mutex_);
        // 一次性错误：失败一次后即可重试成功。
        if (configure_error_) {
            auto error = std::move(*configure_error_);
            configure_error_.reset();
            return std::unexpected(std::move(error));
        }
        return {};
    }

    std::expected<DecodedAudioBatch, AudioDecoderBackendError>
    decode(const EncodedPacket&) override {
        ++decode_calls;
        std::lock_guard lock(mutex_);
        if (decode_error_) {
            auto error = std::move(*decode_error_);
            decode_error_.reset();
            return std::unexpected(std::move(error));
        }
        return decode_output_;
    }

    std::expected<DecodedAudioBatch, AudioDecoderBackendError> drain() override {
        ++drain_calls;
        std::lock_guard lock(mutex_);
        if (drain_error_) {
            auto error = std::move(*drain_error_);
            drain_error_.reset();
            return std::unexpected(std::move(error));
        }
        return drain_output_;
    }

    void reset() noexcept override { ++reset_calls; }

    void unconfigure() noexcept override { ++unconfigure_calls; }

    void set_configure_error(AudioDecoderBackendError error) {
        std::lock_guard lock(mutex_);
        configure_error_ = std::move(error);
    }

    void set_decode_output(DecodedAudioBatch output) {
        std::lock_guard lock(mutex_);
        decode_output_ = std::move(output);
    }

    void set_drain_output(DecodedAudioBatch output) {
        std::lock_guard lock(mutex_);
        drain_output_ = std::move(output);
    }

    void set_decode_error(AudioDecoderBackendError error) {
        std::lock_guard lock(mutex_);
        decode_error_ = std::move(error);
    }

    std::atomic_int configure_calls = 0;
    std::atomic_int decode_calls = 0;
    std::atomic_int drain_calls = 0;
    std::atomic_int reset_calls = 0;
    std::atomic_int unconfigure_calls = 0;

private:
    std::mutex mutex_;
    std::optional<AudioDecoderBackendError> configure_error_;
    std::optional<AudioDecoderBackendError> decode_error_;
    std::optional<AudioDecoderBackendError> drain_error_;
    DecodedAudioBatch decode_output_;
    DecodedAudioBatch drain_output_;
};

class ThrowingAudioDecoderBackend final : public AudioDecoderBackend {
public:
    std::expected<void, AudioDecoderBackendError>
    configure(const contracts::media::AudioCodecConfig&) override {
        throw std::runtime_error("boom");
    }

    std::expected<DecodedAudioBatch, AudioDecoderBackendError>
    decode(const EncodedPacket&) override {
        return DecodedAudioBatch{};
    }

    std::expected<DecodedAudioBatch, AudioDecoderBackendError> drain() override {
        return DecodedAudioBatch{};
    }

    void reset() noexcept override {}

    void unconfigure() noexcept override { ++unconfigure_calls; }

    std::atomic_int unconfigure_calls = 0;
};

class FakeAudioPacketSource final : public AudioPacketSource {
public:
    std::optional<AudioPacketQueueItem> try_pop() override {
        ++pop_calls;
        std::lock_guard lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<AudioPacketQueueItem> item;
        item.emplace(std::move(items_.front()));
        items_.pop_front();
        return item;
    }

    void push(AudioPacketQueueItem item) {
        std::lock_guard lock(mutex_);
        items_.push_back(std::move(item));
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    std::atomic_int pop_calls = 0;

private:
    mutable std::mutex mutex_;
    std::deque<AudioPacketQueueItem> items_;
};

class EmptyAudioPacketSource final : public AudioPacketSource {
public:
    std::optional<AudioPacketQueueItem> try_pop() override {
        ++pop_calls;
        return std::nullopt;
    }

    std::atomic_int pop_calls = 0;
};

class FakeAudioFrameSink final : public AudioFrameSink {
public:
    AudioFramePushResult try_push(AudioFrameStoreItem&& item) override {
        ++push_calls;
        std::lock_guard lock(mutex_);
        if (full_) {
            return AudioFramePushResult::Full;
        }
        items_.push_back(std::move(item));
        return AudioFramePushResult::Accepted;
    }

    void set_full(bool full) {
        std::lock_guard lock(mutex_);
        full_ = full;
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    std::optional<AudioFrameStoreItem> pop() {
        std::lock_guard lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<AudioFrameStoreItem> item;
        item.emplace(std::move(items_.front()));
        items_.pop_front();
        return item;
    }

    std::atomic_int push_calls = 0;

private:
    mutable std::mutex mutex_;
    bool full_ = false;
    std::deque<AudioFrameStoreItem> items_;
};

struct DecoderDependencies {
    std::shared_ptr<AudioPacketSource> source;
    std::shared_ptr<AudioFrameSink> sink;
    std::shared_ptr<AudioDecoderBackend> backend;
    std::shared_ptr<infra::Notifier> notifier;
    std::shared_ptr<Generation> generation;
};

std::unique_ptr<DefaultAudioDecoder> make_decoder(DecoderDependencies dependencies) {
    return std::make_unique<DefaultAudioDecoder>(std::move(dependencies.source),
                                                 std::move(dependencies.sink),
                                                 std::move(dependencies.backend),
                                                 std::move(dependencies.notifier),
                                                 std::move(dependencies.generation));
}

DecoderDependencies complete_dependencies() {
    return DecoderDependencies{
        .source = std::make_shared<FakeAudioPacketSource>(),
        .sink = std::make_shared<FakeAudioFrameSink>(),
        .backend = std::make_shared<FakeAudioDecoderBackend>(),
        .notifier = std::make_shared<infra::DefaultNotifier>(),
        .generation = std::make_shared<Generation>(),
    };
}

EncodedPacket make_encoded_packet(std::uint8_t marker) {
    return EncodedPacket{
        .payload = {std::byte{marker}},
        .pts_us = 10,
        .dts_us = 9,
        .duration_us = 1,
    };
}

AudioPacketQueueItem make_packet_item(std::uint8_t marker, Generation::Value generation) {
    return AudioPacketQueueItem{
        std::in_place_type<AudioPacket>,
        make_encoded_packet(marker),
        generation,
    };
}

contracts::media::DecodedAudio make_decoded_audio(std::uint32_t samples_per_channel) {
    return contracts::media::DecodedAudio{
        .format = contracts::media::AudioPcmFormat{
            .sample_rate = 48000,
            .channels = 2,
            .sample_format = contracts::media::AudioSampleFormat::F32,
            .planar = false,
        },
        .samples_per_channel = samples_per_channel,
        .planes = {{std::byte{0x01}, std::byte{0x02}}},
        .pts_us = 123,
    };
}

template <typename Predicate>
bool eventually(Predicate predicate) {
    using namespace std::chrono_literals;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

TEST(DefaultAudioDecoderTest, OwnsItsWorkerAcrossSessionChanges) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioDecoderBackend>(dependencies.backend);
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_TRUE(configured.has_value());
    EXPECT_EQ(backend->configure_calls, 1);

    decoder->unconfigure();
    EXPECT_EQ(backend->unconfigure_calls, 1);

    // 会话切换不销毁 worker：可再次 configure。
    const auto reconfigured = decoder->configure({});
    ASSERT_TRUE(reconfigured.has_value());
    EXPECT_EQ(backend->configure_calls, 2);
}

TEST(DefaultAudioDecoderTest, RejectsConfigureWhileSessionIsActive) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioDecoderBackend>(dependencies.backend);
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_TRUE(configured.has_value());

    const auto second = decoder->configure({});
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, AudioDecoderErrorCode::InvalidState);
    // 非法的第二次 configure 未触碰 backend。
    EXPECT_EQ(backend->configure_calls, 1);
}

TEST(DefaultAudioDecoderTest, UnconfigureIsIdempotentBeforeAnySession) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioDecoderBackend>(dependencies.backend);
    auto decoder = make_decoder(std::move(dependencies));

    decoder->unconfigure();
    decoder->unconfigure();
    EXPECT_EQ(backend->unconfigure_calls, 0);
}

TEST(DefaultAudioDecoderTest, ReportsBackendConfigureFailureAndRecovers) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioDecoderBackend>(dependencies.backend);
    backend->set_configure_error(AudioDecoderBackendError{
        .operation = AudioDecoderBackendOperation::Configure,
        .native_code = -1094995529,
        .message = "invalid codec",
    });
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, AudioDecoderErrorCode::BackendFailure);
    ASSERT_TRUE(configured.error().backend_error.has_value());
    EXPECT_EQ(configured.error().backend_error->native_code, -1094995529);
    // 失败后 backend 被清理，会话回到 Constructed，可重新 configure。
    EXPECT_EQ(backend->unconfigure_calls, 1);

    const auto retried = decoder->configure({});
    ASSERT_TRUE(retried.has_value());
    EXPECT_EQ(backend->configure_calls, 2);
}

TEST(DefaultAudioDecoderTest, MapsBackendConfigureExceptionToFailure) {
    auto dependencies = complete_dependencies();
    dependencies.backend = std::make_shared<ThrowingAudioDecoderBackend>();
    auto backend = std::static_pointer_cast<ThrowingAudioDecoderBackend>(dependencies.backend);
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, AudioDecoderErrorCode::BackendFailure);
    EXPECT_EQ(backend->unconfigure_calls, 1);
}

TEST(DefaultAudioDecoderTest, RejectsConfigureWhenNotifierIsMissing) {
    // 缺 notifier 时 configure 必须失败：否则数据面将因唤醒源缺失而静默停滞。
    auto dependencies = complete_dependencies();
    dependencies.notifier = nullptr;
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, AudioDecoderErrorCode::InvalidState);
}

TEST(DefaultAudioDecoderTest, RejectsConfigureWhenDependenciesAreMissing) {
    auto decoder = make_decoder(DecoderDependencies{});

    const auto configured = decoder->configure({});
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, AudioDecoderErrorCode::InvalidState);
    EXPECT_FALSE(configured.error().backend_error.has_value());

    // 未建立会话时 unconfigure 幂等返回。
    decoder->unconfigure();
}

TEST(DefaultAudioDecoderTest, JoinsItsWorkerWhenDestroyedWhileConfigured) {
    auto dependencies = complete_dependencies();
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_TRUE(configured.has_value());

    // 直接析构（未 unconfigure）：worker join 收敛；测试挂起即失败。
    decoder.reset();
}

TEST(DefaultAudioDecoderTest, DecodesAudioPacketsIntoFrameSink) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeAudioPacketSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeAudioFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeAudioDecoderBackend>(dependencies.backend);
    backend->set_decode_output(DecodedAudioBatch{make_decoded_audio(32)});
    source->push(make_packet_item(1, dependencies.generation->current()));
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_TRUE(configured.has_value());

    ASSERT_TRUE(eventually([&sink] { return sink->size() == 1; }));
    EXPECT_EQ(backend->decode_calls, 1);
    auto item = sink->pop();
    ASSERT_TRUE(item.has_value());
    const auto* frame = std::get_if<AudioFrame>(&*item);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->generation(), 0);
    EXPECT_EQ(frame->decoded().samples_per_channel, 32);
}

TEST(DefaultAudioDecoderTest, ContinuesReadingAlreadyQueuedPacketsAfterPushingPendingOutput) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeAudioPacketSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeAudioFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeAudioDecoderBackend>(dependencies.backend);
    backend->set_decode_output(DecodedAudioBatch{make_decoded_audio(12)});
    source->push(make_packet_item(1, dependencies.generation->current()));
    source->push(make_packet_item(2, dependencies.generation->current()));
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_TRUE(configured.has_value());

    ASSERT_TRUE(eventually([&sink] { return sink->size() == 2; }));
    EXPECT_EQ(backend->decode_calls, 2);
}

TEST(DefaultAudioDecoderTest, DrainsAndPublishesEndOfInput) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeAudioPacketSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeAudioFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeAudioDecoderBackend>(dependencies.backend);
    backend->set_drain_output(DecodedAudioBatch{make_decoded_audio(16)});
    source->push(AudioPacketEndOfInput{.generation = dependencies.generation->current()});
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_TRUE(configured.has_value());

    ASSERT_TRUE(eventually([&sink] { return sink->size() == 2; }));
    EXPECT_EQ(backend->drain_calls, 1);
    auto frame_item = sink->pop();
    ASSERT_TRUE(frame_item.has_value());
    EXPECT_NE(std::get_if<AudioFrame>(&*frame_item), nullptr);
    auto end_item = sink->pop();
    ASSERT_TRUE(end_item.has_value());
    const auto* end = std::get_if<AudioFrameEndOfInput>(&*end_item);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end->generation, 0);
}

TEST(DefaultAudioDecoderTest, WaitsForOutputNotFullBeforePushingPendingFrame) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeAudioPacketSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeAudioFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeAudioDecoderBackend>(dependencies.backend);
    auto notifier = dependencies.notifier;
    backend->set_decode_output(DecodedAudioBatch{make_decoded_audio(8)});
    sink->set_full(true);
    source->push(make_packet_item(1, dependencies.generation->current()));
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_TRUE(configured.has_value());
    ASSERT_TRUE(eventually([&backend] { return backend->decode_calls.load() == 1; }));
    ASSERT_TRUE(eventually([&sink] { return sink->push_calls.load() == 1; }));
    EXPECT_EQ(sink->size(), 0);

    sink->set_full(false);
    ASSERT_TRUE(notifier->send(AudioFrameStoreNotFull{}));

    ASSERT_TRUE(eventually([&sink] { return sink->size() == 1; }));
    EXPECT_GE(sink->push_calls.load(), 2);
}

TEST(DefaultAudioDecoderTest, ResetsBackendAndDropsStalePacketsWhenGenerationChanges) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeAudioPacketSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeAudioFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeAudioDecoderBackend>(dependencies.backend);
    auto generation = dependencies.generation;
    auto notifier = dependencies.notifier;
    backend->set_decode_output(DecodedAudioBatch{make_decoded_audio(4)});
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_TRUE(configured.has_value());
    ASSERT_TRUE(eventually([&source] { return source->pop_calls.load() > 0; }));

    generation->bump();
    source->push(make_packet_item(1, 0));
    source->push(make_packet_item(2, generation->current()));
    ASSERT_TRUE(notifier->send(AudioQueueNotEmpty{}));

    ASSERT_TRUE(eventually([&sink] { return sink->size() == 1; }));
    EXPECT_EQ(backend->reset_calls, 1);
    EXPECT_EQ(backend->decode_calls, 1);
    auto item = sink->pop();
    ASSERT_TRUE(item.has_value());
    const auto* frame = std::get_if<AudioFrame>(&*item);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->generation(), generation->current());
}

TEST(DefaultAudioDecoderTest, ReportsDecodeFailureAndRequiresUnconfigureForRecovery) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeAudioPacketSource>(dependencies.source);
    auto backend = std::static_pointer_cast<FakeAudioDecoderBackend>(dependencies.backend);
    auto notifier = dependencies.notifier;
    std::atomic_int failure_events = 0;
    auto failure_subscription = notifier->subscribe<AudioDecoderBackendFailure>(
        [&failure_events](const AudioDecoderBackendFailure&) {
            ++failure_events;
        });
    backend->set_decode_error(AudioDecoderBackendError{
        .operation = AudioDecoderBackendOperation::Decode,
        .native_code = -1,
        .message = "decode failed",
    });
    source->push(make_packet_item(1, dependencies.generation->current()));
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_TRUE(configured.has_value());
    ASSERT_TRUE(eventually([&failure_events] { return failure_events.load() == 1; }));

    const auto reconfigure_while_failed = decoder->configure({});
    ASSERT_FALSE(reconfigure_while_failed.has_value());
    EXPECT_EQ(reconfigure_while_failed.error().code, AudioDecoderErrorCode::InvalidState);

    decoder->unconfigure();
    const auto reconfigured = decoder->configure({});
    EXPECT_TRUE(reconfigured.has_value());
}

} // namespace
} // namespace semi::domain
