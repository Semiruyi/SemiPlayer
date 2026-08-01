#include "domain/worker/audio_decoder/default_audio_decoder.hpp"

#include "contracts/audio_decoder/audio_decoder_backend.hpp"
#include "domain/resource/audio_frame_store/audio_frame_sink.hpp"
#include "domain/resource/audio_packet_queue/audio_packet_source.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_decoder/audio_decoder.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

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
        return DecodedAudioBatch{};
    }

    std::expected<DecodedAudioBatch, AudioDecoderBackendError> drain() override {
        ++drain_calls;
        return DecodedAudioBatch{};
    }

    void reset() noexcept override { ++reset_calls; }

    void unconfigure() noexcept override { ++unconfigure_calls; }

    void set_configure_error(AudioDecoderBackendError error) {
        std::lock_guard lock(mutex_);
        configure_error_ = std::move(error);
    }

    std::atomic_int configure_calls = 0;
    std::atomic_int decode_calls = 0;
    std::atomic_int drain_calls = 0;
    std::atomic_int reset_calls = 0;
    std::atomic_int unconfigure_calls = 0;

private:
    std::mutex mutex_;
    std::optional<AudioDecoderBackendError> configure_error_;
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
        return std::nullopt;
    }

    std::atomic_int pop_calls = 0;
};

class FakeAudioFrameSink final : public AudioFrameSink {
public:
    AudioFramePushResult try_push(AudioFrameStoreItem&&) override {
        ++push_calls;
        return AudioFramePushResult::Accepted;
    }

    std::atomic_int push_calls = 0;
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

} // namespace
} // namespace semi::domain
