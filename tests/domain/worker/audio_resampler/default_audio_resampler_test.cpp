#include "domain/worker/audio_resampler/default_audio_resampler.hpp"

#include "contracts/audio_resampler/audio_resampler_backend.hpp"
#include "domain/resource/audio_frame_store/audio_frame_sink.hpp"
#include "domain/resource/audio_frame_store/audio_frame_source.hpp"
#include "domain/resource/audio_frame_store/audio_frame_store_events.hpp"
#include "domain/resource/audio_frame_store/audio_frame_store_item.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_resampler/audio_resampler.hpp"
#include "domain/worker/audio_resampler/audio_resampler_events.hpp"
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

using contracts::audio_resampler::AudioResamplerBackend;
using contracts::audio_resampler::AudioResamplerBackendError;
using contracts::audio_resampler::AudioResamplerBackendOperation;
using contracts::audio_resampler::ResampledAudioBatch;

class FakeAudioResamplerBackend final : public AudioResamplerBackend {
public:
    std::expected<void, AudioResamplerBackendError>
    configure(const contracts::media::AudioPcmFormat& input_format,
              const contracts::media::AudioPcmFormat& output_format) override {
        ++configure_calls;
        std::lock_guard lock(mutex_);
        last_input_format = input_format;
        last_output_format = output_format;
        if (configure_error_) {
            auto error = std::move(*configure_error_);
            configure_error_.reset();
            return std::unexpected(std::move(error));
        }
        return {};
    }

    std::expected<ResampledAudioBatch, AudioResamplerBackendError>
    resample(const contracts::media::DecodedAudio&) override {
        ++resample_calls;
        std::lock_guard lock(mutex_);
        if (resample_error_) {
            auto error = std::move(*resample_error_);
            resample_error_.reset();
            return std::unexpected(std::move(error));
        }
        return resample_output_;
    }

    std::expected<ResampledAudioBatch, AudioResamplerBackendError> drain() override {
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

    void set_configure_error(AudioResamplerBackendError error) {
        std::lock_guard lock(mutex_);
        configure_error_ = std::move(error);
    }

    void set_resample_output(ResampledAudioBatch output) {
        std::lock_guard lock(mutex_);
        resample_output_ = std::move(output);
    }

    void set_drain_output(ResampledAudioBatch output) {
        std::lock_guard lock(mutex_);
        drain_output_ = std::move(output);
    }

    void set_resample_error(AudioResamplerBackendError error) {
        std::lock_guard lock(mutex_);
        resample_error_ = std::move(error);
    }

    std::atomic_int configure_calls = 0;
    std::atomic_int resample_calls = 0;
    std::atomic_int drain_calls = 0;
    std::atomic_int reset_calls = 0;
    std::atomic_int unconfigure_calls = 0;
    contracts::media::AudioPcmFormat last_input_format;
    contracts::media::AudioPcmFormat last_output_format;

private:
    std::mutex mutex_;
    std::optional<AudioResamplerBackendError> configure_error_;
    std::optional<AudioResamplerBackendError> resample_error_;
    std::optional<AudioResamplerBackendError> drain_error_;
    ResampledAudioBatch resample_output_;
    ResampledAudioBatch drain_output_;
};

class ThrowingAudioResamplerBackend final : public AudioResamplerBackend {
public:
    std::expected<void, AudioResamplerBackendError>
    configure(const contracts::media::AudioPcmFormat&,
              const contracts::media::AudioPcmFormat&) override {
        throw std::runtime_error("boom");
    }

    std::expected<ResampledAudioBatch, AudioResamplerBackendError>
    resample(const contracts::media::DecodedAudio&) override {
        return ResampledAudioBatch{};
    }

    std::expected<ResampledAudioBatch, AudioResamplerBackendError> drain() override {
        return ResampledAudioBatch{};
    }

    void reset() noexcept override {}

    void unconfigure() noexcept override { ++unconfigure_calls; }

    std::atomic_int unconfigure_calls = 0;
};

class FakeAudioFrameSource final : public AudioFrameSource {
public:
    std::optional<AudioFrameStoreItem> try_pop() override {
        ++pop_calls;
        std::lock_guard lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<AudioFrameStoreItem> item;
        item.emplace(std::move(items_.front()));
        items_.pop_front();
        return item;
    }

    void push(AudioFrameStoreItem item) {
        std::lock_guard lock(mutex_);
        items_.push_back(std::move(item));
    }

    std::atomic_int pop_calls = 0;

private:
    std::mutex mutex_;
    std::deque<AudioFrameStoreItem> items_;
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

struct ResamplerDependencies {
    std::shared_ptr<AudioFrameSource> source;
    std::shared_ptr<AudioFrameSink> sink;
    std::shared_ptr<AudioResamplerBackend> backend;
    std::shared_ptr<infra::Notifier> notifier;
    std::shared_ptr<Generation> generation;
};

std::unique_ptr<DefaultAudioResampler> make_resampler(ResamplerDependencies dependencies) {
    return std::make_unique<DefaultAudioResampler>(std::move(dependencies.source),
                                                   std::move(dependencies.sink),
                                                   std::move(dependencies.backend),
                                                   std::move(dependencies.notifier),
                                                   std::move(dependencies.generation));
}

ResamplerDependencies complete_dependencies() {
    return ResamplerDependencies{
        .source = std::make_shared<FakeAudioFrameSource>(),
        .sink = std::make_shared<FakeAudioFrameSink>(),
        .backend = std::make_shared<FakeAudioResamplerBackend>(),
        .notifier = std::make_shared<infra::DefaultNotifier>(),
        .generation = std::make_shared<Generation>(),
    };
}

contracts::media::AudioPcmFormat make_input_format() {
    return contracts::media::AudioPcmFormat{
        .sample_rate = 44100,
        .channels = 2,
        .sample_format = contracts::media::AudioSampleFormat::S16,
        .planar = true,
    };
}

contracts::media::AudioPcmFormat make_output_format() {
    return contracts::media::AudioPcmFormat{
        .sample_rate = 48000,
        .channels = 2,
        .sample_format = contracts::media::AudioSampleFormat::F32,
        .planar = false,
    };
}

contracts::media::DecodedAudio make_pcm(std::uint32_t samples_per_channel) {
    return contracts::media::DecodedAudio{
        .format = make_output_format(),
        .samples_per_channel = samples_per_channel,
        .planes = {{std::byte{0x01}, std::byte{0x02}}},
        .pts_us = 123,
    };
}

AudioFrameStoreItem make_frame_item(std::uint32_t samples_per_channel,
                                    Generation::Value generation) {
    return AudioFrameStoreItem{
        std::in_place_type<AudioFrame>,
        make_pcm(samples_per_channel),
        generation,
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

TEST(DefaultAudioResamplerTest, OwnsItsWorkerAcrossSessionChanges) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioResamplerBackend>(dependencies.backend);
    auto resampler = make_resampler(std::move(dependencies));

    const auto configured = resampler->configure(make_input_format(), make_output_format());
    ASSERT_TRUE(configured.has_value());
    EXPECT_EQ(backend->configure_calls, 1);
    EXPECT_EQ(backend->last_input_format.sample_rate, 44100U);
    EXPECT_EQ(backend->last_output_format.sample_rate, 48000U);

    resampler->unconfigure();
    EXPECT_EQ(backend->unconfigure_calls, 1);

    const auto reconfigured = resampler->configure(make_input_format(), make_output_format());
    ASSERT_TRUE(reconfigured.has_value());
    EXPECT_EQ(backend->configure_calls, 2);
}

TEST(DefaultAudioResamplerTest, RejectsConfigureWhileSessionIsActive) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioResamplerBackend>(dependencies.backend);
    auto resampler = make_resampler(std::move(dependencies));

    const auto configured = resampler->configure(make_input_format(), make_output_format());
    ASSERT_TRUE(configured.has_value());

    const auto second = resampler->configure(make_input_format(), make_output_format());
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, AudioResamplerErrorCode::InvalidState);
    EXPECT_EQ(backend->configure_calls, 1);
}

TEST(DefaultAudioResamplerTest, UnconfigureIsIdempotentBeforeAnySession) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioResamplerBackend>(dependencies.backend);
    auto resampler = make_resampler(std::move(dependencies));

    resampler->unconfigure();
    resampler->unconfigure();
    EXPECT_EQ(backend->unconfigure_calls, 0);
}

TEST(DefaultAudioResamplerTest, ReportsBackendConfigureFailureAndRecovers) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioResamplerBackend>(dependencies.backend);
    backend->set_configure_error(AudioResamplerBackendError{
        .operation = AudioResamplerBackendOperation::Configure,
        .native_code = -22,
        .message = "unsupported format",
    });
    auto resampler = make_resampler(std::move(dependencies));

    const auto configured = resampler->configure(make_input_format(), make_output_format());
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, AudioResamplerErrorCode::BackendFailure);
    ASSERT_TRUE(configured.error().backend_error.has_value());
    EXPECT_EQ(configured.error().backend_error->native_code, -22);
    EXPECT_EQ(backend->unconfigure_calls, 1);

    const auto retried = resampler->configure(make_input_format(), make_output_format());
    ASSERT_TRUE(retried.has_value());
    EXPECT_EQ(backend->configure_calls, 2);
}

TEST(DefaultAudioResamplerTest, MapsBackendConfigureExceptionToFailure) {
    auto dependencies = complete_dependencies();
    dependencies.backend = std::make_shared<ThrowingAudioResamplerBackend>();
    auto backend = std::static_pointer_cast<ThrowingAudioResamplerBackend>(dependencies.backend);
    auto resampler = make_resampler(std::move(dependencies));

    const auto configured = resampler->configure(make_input_format(), make_output_format());
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, AudioResamplerErrorCode::BackendFailure);
    EXPECT_EQ(backend->unconfigure_calls, 1);
}

TEST(DefaultAudioResamplerTest, RejectsConfigureWhenNotifierIsMissing) {
    auto dependencies = complete_dependencies();
    dependencies.notifier = nullptr;
    auto resampler = make_resampler(std::move(dependencies));

    const auto configured = resampler->configure(make_input_format(), make_output_format());
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, AudioResamplerErrorCode::InvalidState);
}

TEST(DefaultAudioResamplerTest, RejectsConfigureWhenDependenciesAreMissing) {
    auto resampler = make_resampler(ResamplerDependencies{});

    const auto configured = resampler->configure(make_input_format(), make_output_format());
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, AudioResamplerErrorCode::InvalidState);
    EXPECT_FALSE(configured.error().backend_error.has_value());

    resampler->unconfigure();
}

TEST(DefaultAudioResamplerTest, JoinsItsWorkerWhenDestroyedWhileConfigured) {
    auto dependencies = complete_dependencies();
    auto resampler = make_resampler(std::move(dependencies));

    const auto configured = resampler->configure(make_input_format(), make_output_format());
    ASSERT_TRUE(configured.has_value());

    resampler.reset();
}

TEST(DefaultAudioResamplerTest, ResamplesFramesIntoOutputFrameSink) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeAudioFrameSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeAudioFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeAudioResamplerBackend>(dependencies.backend);
    backend->set_resample_output(ResampledAudioBatch{make_pcm(32)});
    source->push(make_frame_item(16, dependencies.generation->current()));
    auto resampler = make_resampler(std::move(dependencies));

    const auto configured = resampler->configure(make_input_format(), make_output_format());
    ASSERT_TRUE(configured.has_value());

    ASSERT_TRUE(eventually([&sink] { return sink->size() == 1; }));
    EXPECT_EQ(backend->resample_calls, 1);
    auto item = sink->pop();
    ASSERT_TRUE(item.has_value());
    const auto* frame = std::get_if<AudioFrame>(&*item);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->generation(), 0);
    EXPECT_EQ(frame->decoded().samples_per_channel, 32);
}

TEST(DefaultAudioResamplerTest, ContinuesReadingAlreadyQueuedFramesAfterPushingPendingOutput) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeAudioFrameSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeAudioFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeAudioResamplerBackend>(dependencies.backend);
    backend->set_resample_output(ResampledAudioBatch{make_pcm(12)});
    source->push(make_frame_item(1, dependencies.generation->current()));
    source->push(make_frame_item(2, dependencies.generation->current()));
    auto resampler = make_resampler(std::move(dependencies));

    const auto configured = resampler->configure(make_input_format(), make_output_format());
    ASSERT_TRUE(configured.has_value());

    ASSERT_TRUE(eventually([&sink] { return sink->size() == 2; }));
    EXPECT_EQ(backend->resample_calls, 2);
}

TEST(DefaultAudioResamplerTest, DrainsAndPublishesEndOfInput) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeAudioFrameSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeAudioFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeAudioResamplerBackend>(dependencies.backend);
    backend->set_drain_output(ResampledAudioBatch{make_pcm(16)});
    source->push(AudioFrameEndOfInput{.generation = dependencies.generation->current()});
    auto resampler = make_resampler(std::move(dependencies));

    const auto configured = resampler->configure(make_input_format(), make_output_format());
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

TEST(DefaultAudioResamplerTest, WaitsForOutputNotFullBeforePushingPendingFrame) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeAudioFrameSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeAudioFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeAudioResamplerBackend>(dependencies.backend);
    auto notifier = dependencies.notifier;
    backend->set_resample_output(ResampledAudioBatch{make_pcm(8)});
    sink->set_full(true);
    source->push(make_frame_item(1, dependencies.generation->current()));
    auto resampler = make_resampler(std::move(dependencies));

    const auto configured = resampler->configure(make_input_format(), make_output_format());
    ASSERT_TRUE(configured.has_value());
    ASSERT_TRUE(eventually([&backend] { return backend->resample_calls.load() == 1; }));
    ASSERT_TRUE(eventually([&sink] { return sink->push_calls.load() == 1; }));
    EXPECT_EQ(sink->size(), 0);

    sink->set_full(false);
    ASSERT_TRUE(notifier->send(AudioFrameStoreNotFull{}));

    ASSERT_TRUE(eventually([&sink] { return sink->size() == 1; }));
    EXPECT_GE(sink->push_calls.load(), 2);
}

TEST(DefaultAudioResamplerTest, ResetsBackendAndDropsStaleFramesWhenGenerationChanges) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeAudioFrameSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeAudioFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeAudioResamplerBackend>(dependencies.backend);
    auto generation = dependencies.generation;
    auto notifier = dependencies.notifier;
    backend->set_resample_output(ResampledAudioBatch{make_pcm(4)});
    auto resampler = make_resampler(std::move(dependencies));

    const auto configured = resampler->configure(make_input_format(), make_output_format());
    ASSERT_TRUE(configured.has_value());
    ASSERT_TRUE(eventually([&source] { return source->pop_calls.load() > 0; }));

    generation->bump();
    source->push(make_frame_item(1, 0));
    source->push(make_frame_item(2, generation->current()));
    ASSERT_TRUE(notifier->send(AudioFrameStoreNotEmpty{}));

    ASSERT_TRUE(eventually([&sink] { return sink->size() == 1; }));
    EXPECT_EQ(backend->reset_calls, 1);
    EXPECT_EQ(backend->resample_calls, 1);
    auto item = sink->pop();
    ASSERT_TRUE(item.has_value());
    const auto* frame = std::get_if<AudioFrame>(&*item);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->generation(), generation->current());
}

TEST(DefaultAudioResamplerTest, ReportsResampleFailureAndRequiresUnconfigureForRecovery) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeAudioFrameSource>(dependencies.source);
    auto backend = std::static_pointer_cast<FakeAudioResamplerBackend>(dependencies.backend);
    auto notifier = dependencies.notifier;
    std::atomic_int failure_events = 0;
    auto failure_subscription = notifier->subscribe<AudioResamplerBackendFailure>(
        [&failure_events](const AudioResamplerBackendFailure&) {
            ++failure_events;
        });
    backend->set_resample_error(AudioResamplerBackendError{
        .operation = AudioResamplerBackendOperation::Resample,
        .native_code = -1,
        .message = "resample failed",
    });
    source->push(make_frame_item(1, dependencies.generation->current()));
    auto resampler = make_resampler(std::move(dependencies));

    const auto configured = resampler->configure(make_input_format(), make_output_format());
    ASSERT_TRUE(configured.has_value());
    ASSERT_TRUE(eventually([&failure_events] { return failure_events.load() == 1; }));

    const auto reconfigure_while_failed =
        resampler->configure(make_input_format(), make_output_format());
    ASSERT_FALSE(reconfigure_while_failed.has_value());
    EXPECT_EQ(reconfigure_while_failed.error().code, AudioResamplerErrorCode::InvalidState);

    resampler->unconfigure();
    const auto reconfigured = resampler->configure(make_input_format(), make_output_format());
    EXPECT_TRUE(reconfigured.has_value());
}

} // namespace
} // namespace semi::domain
