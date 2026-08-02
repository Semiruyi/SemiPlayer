#include "domain/worker/audio_output/default_audio_output.hpp"

#include "domain/resource/audio_frame_store/audio_frame_source.hpp"
#include "domain/resource/audio_frame_store/audio_frame_store_events.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_output/audio_output_events.hpp"
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

contracts::media::AudioPcmFormat playback_format() {
    return contracts::media::AudioPcmFormat{
        .sample_rate = 48000,
        .channels = 2,
        .sample_format = contracts::media::AudioSampleFormat::F32,
        .planar = false,
    };
}

contracts::media::DecodedAudio make_decoded_audio(std::uint32_t marker) {
    return contracts::media::DecodedAudio{
        .format = playback_format(),
        .samples_per_channel = 1,
        .planes = {{std::byte{static_cast<std::uint8_t>(marker)}}},
        .pts_us = static_cast<std::int64_t>(marker),
    };
}

AudioFrameStoreItem make_frame_item(std::uint32_t marker, Generation::Value generation) {
    return AudioFrameStoreItem{
        std::in_place_type<AudioFrame>,
        make_decoded_audio(marker),
        generation,
    };
}

AudioFrameStoreItem make_end_item(Generation::Value generation) {
    return AudioFrameStoreItem{AudioFrameEndOfInput{.generation = generation}};
}

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

class FakeAudioOutputBackend final : public AudioOutputBackend {
public:
    void set_progress_notifier(
        contracts::audio_output::AudioOutputBackendProgressNotifier* notifier) noexcept override {
        progress_notifier = notifier;
    }

    std::expected<AudioOutputConfigureResult, AudioOutputBackendError>
    configure(const AudioOutputOptions& options) override {
        ++configure_calls;
        std::lock_guard lock(mutex_);
        last_options = options;
        if (configure_error_) {
            auto error = std::move(*configure_error_);
            configure_error_.reset();
            return std::unexpected(std::move(error));
        }
        return AudioOutputConfigureResult{.playback_format = format};
    }

    std::expected<AudioOutputSubmitStatus, AudioOutputBackendError>
    try_submit(const contracts::media::DecodedAudio& audio) override {
        ++submit_calls;
        std::lock_guard lock(mutex_);
        if (submit_error_) {
            auto error = std::move(*submit_error_);
            submit_error_.reset();
            return std::unexpected(std::move(error));
        }
        if (!submit_results_.empty()) {
            const auto result = submit_results_.front();
            submit_results_.pop_front();
            if (result == AudioOutputSubmitStatus::Accepted) {
                submitted_markers.push_back(audio.planes.front().front());
            }
            return result;
        }
        submitted_markers.push_back(audio.planes.front().front());
        return AudioOutputSubmitStatus::Accepted;
    }

    std::expected<AudioOutputDrainStatus, AudioOutputBackendError> try_drain() override {
        ++drain_calls;
        std::lock_guard lock(mutex_);
        if (drain_error_) {
            auto error = std::move(*drain_error_);
            drain_error_.reset();
            return std::unexpected(std::move(error));
        }
        if (!drain_results_.empty()) {
            const auto result = drain_results_.front();
            drain_results_.pop_front();
            return result;
        }
        return AudioOutputDrainStatus::Drained;
    }

    void reset() noexcept override { ++reset_calls; }
    void unconfigure() noexcept override { ++unconfigure_calls; }

    void push_submit_result(AudioOutputSubmitStatus status) {
        std::lock_guard lock(mutex_);
        submit_results_.push_back(status);
    }

    void push_drain_result(AudioOutputDrainStatus status) {
        std::lock_guard lock(mutex_);
        drain_results_.push_back(status);
    }

    void set_configure_error(AudioOutputBackendError error) {
        std::lock_guard lock(mutex_);
        configure_error_ = std::move(error);
    }

    void set_submit_error(AudioOutputBackendError error) {
        std::lock_guard lock(mutex_);
        submit_error_ = std::move(error);
    }

    void set_drain_error(AudioOutputBackendError error) {
        std::lock_guard lock(mutex_);
        drain_error_ = std::move(error);
    }

    void notify_progress() {
        if (progress_notifier != nullptr) {
            progress_notifier->notify_audio_output_progress_available();
        }
    }

    std::size_t submitted_marker_count() const {
        std::lock_guard lock(mutex_);
        return submitted_markers.size();
    }

    std::byte submitted_marker_at(std::size_t index) const {
        std::lock_guard lock(mutex_);
        return submitted_markers.at(index);
    }

    std::atomic_int configure_calls = 0;
    std::atomic_int submit_calls = 0;
    std::atomic_int drain_calls = 0;
    std::atomic_int reset_calls = 0;
    std::atomic_int unconfigure_calls = 0;
    contracts::media::AudioPcmFormat format = playback_format();
    AudioOutputOptions last_options;
    std::vector<std::byte> submitted_markers;
    contracts::audio_output::AudioOutputBackendProgressNotifier* progress_notifier = nullptr;

private:
    mutable std::mutex mutex_;
    std::optional<AudioOutputBackendError> configure_error_;
    std::optional<AudioOutputBackendError> submit_error_;
    std::optional<AudioOutputBackendError> drain_error_;
    std::deque<AudioOutputSubmitStatus> submit_results_;
    std::deque<AudioOutputDrainStatus> drain_results_;
};

class ThrowingAudioOutputBackend final : public AudioOutputBackend {
public:
    void set_progress_notifier(
        contracts::audio_output::AudioOutputBackendProgressNotifier*) noexcept override {}

    std::expected<AudioOutputConfigureResult, AudioOutputBackendError>
    configure(const AudioOutputOptions&) override {
        throw std::runtime_error("boom");
    }

    std::expected<AudioOutputSubmitStatus, AudioOutputBackendError>
    try_submit(const contracts::media::DecodedAudio&) override {
        return AudioOutputSubmitStatus::Accepted;
    }

    std::expected<AudioOutputDrainStatus, AudioOutputBackendError> try_drain() override {
        return AudioOutputDrainStatus::Drained;
    }

    void reset() noexcept override {}
    void unconfigure() noexcept override { ++unconfigure_calls; }

    std::atomic_int unconfigure_calls = 0;
};

struct OutputDependencies {
    std::shared_ptr<FakeAudioFrameSource> source;
    std::shared_ptr<AudioOutputBackend> backend;
    std::shared_ptr<infra::DefaultNotifier> notifier;
    std::shared_ptr<Generation> generation;
};

OutputDependencies complete_dependencies() {
    return OutputDependencies{
        .source = std::make_shared<FakeAudioFrameSource>(),
        .backend = std::make_shared<FakeAudioOutputBackend>(),
        .notifier = std::make_shared<infra::DefaultNotifier>(),
        .generation = std::make_shared<Generation>(),
    };
}

std::unique_ptr<DefaultAudioOutput> make_output(const OutputDependencies& dependencies) {
    return std::make_unique<DefaultAudioOutput>(
        dependencies.source,
        dependencies.backend,
        dependencies.notifier,
        dependencies.generation);
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

TEST(DefaultAudioOutputTest, OwnsItsWorkerAcrossSessionChanges) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioOutputBackend>(dependencies.backend);
    auto output = make_output(dependencies);

    const auto configured = output->configure(AudioOutputOptions{.device_id = "default"});
    ASSERT_TRUE(configured.has_value());
    EXPECT_EQ(configured->playback_format.sample_rate, backend->format.sample_rate);
    ASSERT_TRUE(backend->last_options.device_id.has_value());
    EXPECT_EQ(*backend->last_options.device_id, "default");
    EXPECT_EQ(backend->configure_calls, 1);

    output->unconfigure();
    EXPECT_EQ(backend->unconfigure_calls, 1);

    const auto reconfigured = output->configure({});
    ASSERT_TRUE(reconfigured.has_value());
    EXPECT_EQ(backend->configure_calls, 2);
}

TEST(DefaultAudioOutputTest, RejectsConfigureWhileSessionIsActive) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioOutputBackend>(dependencies.backend);
    auto output = make_output(dependencies);

    const auto configured = output->configure({});
    ASSERT_TRUE(configured.has_value());

    const auto second = output->configure({});
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, AudioOutputErrorCode::InvalidState);
    EXPECT_EQ(backend->configure_calls, 1);
}

TEST(DefaultAudioOutputTest, ReportsBackendConfigureFailureAndRecovers) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioOutputBackend>(dependencies.backend);
    backend->set_configure_error(AudioOutputBackendError{
        .operation = AudioOutputBackendOperation::Configure,
        .native_code = -1,
        .message = "device open failed",
    });
    auto output = make_output(dependencies);

    const auto configured = output->configure({});
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, AudioOutputErrorCode::BackendFailure);
    EXPECT_TRUE(configured.error().backend_error.has_value());
    EXPECT_EQ(backend->unconfigure_calls, 1);

    const auto retried = output->configure({});
    ASSERT_TRUE(retried.has_value());
    EXPECT_EQ(backend->configure_calls, 2);
}

TEST(DefaultAudioOutputTest, MapsBackendConfigureExceptionToFailure) {
    auto dependencies = complete_dependencies();
    dependencies.backend = std::make_shared<ThrowingAudioOutputBackend>();
    auto backend = std::static_pointer_cast<ThrowingAudioOutputBackend>(dependencies.backend);
    auto output = make_output(dependencies);

    const auto configured = output->configure({});

    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, AudioOutputErrorCode::BackendFailure);
    EXPECT_EQ(backend->unconfigure_calls, 1);
}

TEST(DefaultAudioOutputTest, SubmitsPlaybackFramesToBackend) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioOutputBackend>(dependencies.backend);
    auto output = make_output(dependencies);

    ASSERT_TRUE(output->configure({}).has_value());
    ASSERT_TRUE(output->start_playback().has_value());
    dependencies.source->push(make_frame_item(1, dependencies.generation->current()));
    ASSERT_TRUE(dependencies.notifier->send(AudioFrameStoreNotEmpty{}));

    ASSERT_TRUE(eventually([&] { return backend->submitted_marker_count() == 1; }));
    EXPECT_EQ(backend->submitted_marker_at(0), std::byte{0x01});
}

TEST(DefaultAudioOutputTest, WaitsForStartPlaybackBeforeConsumingFrames) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioOutputBackend>(dependencies.backend);
    auto output = make_output(dependencies);

    ASSERT_TRUE(output->configure({}).has_value());
    dependencies.source->push(make_frame_item(6, dependencies.generation->current()));
    ASSERT_TRUE(dependencies.notifier->send(AudioFrameStoreNotEmpty{}));

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(backend->submit_calls, 0);

    ASSERT_TRUE(output->start_playback().has_value());

    ASSERT_TRUE(eventually([&] { return backend->submitted_marker_count() == 1; }));
    EXPECT_EQ(backend->submitted_marker_at(0), std::byte{0x06});
}

TEST(DefaultAudioOutputTest, PausePlaybackStopsConsumingUntilRestarted) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioOutputBackend>(dependencies.backend);
    auto output = make_output(dependencies);

    ASSERT_TRUE(output->configure({}).has_value());
    ASSERT_TRUE(output->start_playback().has_value());
    output->pause_playback();

    dependencies.source->push(make_frame_item(7, dependencies.generation->current()));
    ASSERT_TRUE(dependencies.notifier->send(AudioFrameStoreNotEmpty{}));

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(backend->submit_calls, 0);

    ASSERT_TRUE(output->start_playback().has_value());

    ASSERT_TRUE(eventually([&] { return backend->submitted_marker_count() == 1; }));
    EXPECT_EQ(backend->submitted_marker_at(0), std::byte{0x07});
}

TEST(DefaultAudioOutputTest, KeepsPendingFrameUntilBackendProgressAfterWouldBlock) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioOutputBackend>(dependencies.backend);
    backend->push_submit_result(AudioOutputSubmitStatus::WouldBlock);
    backend->push_submit_result(AudioOutputSubmitStatus::Accepted);
    auto output = make_output(dependencies);

    ASSERT_TRUE(output->configure({}).has_value());
    ASSERT_TRUE(output->start_playback().has_value());
    dependencies.source->push(make_frame_item(2, dependencies.generation->current()));
    ASSERT_TRUE(dependencies.notifier->send(AudioFrameStoreNotEmpty{}));

    ASSERT_TRUE(eventually([&] { return backend->submit_calls.load() == 1; }));
    EXPECT_EQ(backend->submitted_marker_count(), 0U);

    backend->notify_progress();

    ASSERT_TRUE(eventually([&] { return backend->submitted_marker_count() == 1; }));
    EXPECT_EQ(backend->submitted_marker_at(0), std::byte{0x02});
}

TEST(DefaultAudioOutputTest, DrainsAfterEndOfInputBeforeSendingPlaybackFinished) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioOutputBackend>(dependencies.backend);
    backend->push_drain_result(AudioOutputDrainStatus::WouldBlock);
    backend->push_drain_result(AudioOutputDrainStatus::Drained);
    std::vector<AudioPlaybackFinished> finished_events;
    auto subscription = dependencies.notifier->subscribe<AudioPlaybackFinished>(
        [&finished_events](const AudioPlaybackFinished& event) {
            finished_events.push_back(event);
        });
    auto output = make_output(dependencies);

    ASSERT_TRUE(output->configure({}).has_value());
    ASSERT_TRUE(output->start_playback().has_value());
    dependencies.source->push(make_end_item(dependencies.generation->current()));
    ASSERT_TRUE(dependencies.notifier->send(AudioFrameStoreNotEmpty{}));

    ASSERT_TRUE(eventually([&] { return backend->drain_calls.load() == 1; }));
    EXPECT_TRUE(finished_events.empty());

    backend->notify_progress();

    ASSERT_TRUE(eventually([&] { return !finished_events.empty(); }));
    EXPECT_EQ(finished_events.front().generation, dependencies.generation->current());
}

TEST(DefaultAudioOutputTest, DropsStaleEndOfInputWithoutFinishingPlayback) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioOutputBackend>(dependencies.backend);
    std::vector<AudioPlaybackFinished> finished_events;
    auto subscription = dependencies.notifier->subscribe<AudioPlaybackFinished>(
        [&finished_events](const AudioPlaybackFinished& event) {
            finished_events.push_back(event);
        });
    auto output = make_output(dependencies);

    ASSERT_TRUE(output->configure({}).has_value());
    ASSERT_TRUE(output->start_playback().has_value());
    dependencies.generation->bump();
    dependencies.source->push(make_end_item(0));
    ASSERT_TRUE(dependencies.notifier->send(AudioFrameStoreNotEmpty{}));

    ASSERT_TRUE(eventually([&] { return dependencies.source->pop_calls.load() >= 1; }));
    EXPECT_EQ(backend->drain_calls, 0);
    EXPECT_TRUE(finished_events.empty());
}

TEST(DefaultAudioOutputTest, ResetsBackendAndDropsPendingFrameWhenGenerationChanges) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioOutputBackend>(dependencies.backend);
    backend->push_submit_result(AudioOutputSubmitStatus::WouldBlock);
    backend->push_submit_result(AudioOutputSubmitStatus::Accepted);
    auto output = make_output(dependencies);

    ASSERT_TRUE(output->configure({}).has_value());
    ASSERT_TRUE(output->start_playback().has_value());
    dependencies.source->push(make_frame_item(3, dependencies.generation->current()));
    ASSERT_TRUE(dependencies.notifier->send(AudioFrameStoreNotEmpty{}));
    ASSERT_TRUE(eventually([&] { return backend->submit_calls.load() == 1; }));

    dependencies.generation->bump();
    dependencies.source->push(make_frame_item(4, dependencies.generation->current()));
    ASSERT_TRUE(dependencies.notifier->send(AudioFrameStoreNotEmpty{}));
    backend->notify_progress();

    ASSERT_TRUE(eventually([&] { return backend->reset_calls.load() == 1; }));
    ASSERT_TRUE(eventually([&] { return backend->submit_calls.load() == 2; }));
    ASSERT_EQ(backend->submitted_marker_count(), 1U);
    EXPECT_EQ(backend->submitted_marker_at(0), std::byte{0x04});
}

TEST(DefaultAudioOutputTest, ReportsSubmitFailureAndRequiresUnconfigureForRecovery) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeAudioOutputBackend>(dependencies.backend);
    std::vector<AudioOutputBackendFailure> failure_events;
    auto failure_subscription = dependencies.notifier->subscribe<AudioOutputBackendFailure>(
        [&failure_events](const AudioOutputBackendFailure& event) {
            failure_events.push_back(event);
        });
    backend->set_submit_error(AudioOutputBackendError{
        .operation = AudioOutputBackendOperation::Submit,
        .native_code = -5,
        .message = "submit failed",
    });
    auto output = make_output(dependencies);

    ASSERT_TRUE(output->configure({}).has_value());
    ASSERT_TRUE(output->start_playback().has_value());
    dependencies.source->push(make_frame_item(5, dependencies.generation->current()));
    ASSERT_TRUE(dependencies.notifier->send(AudioFrameStoreNotEmpty{}));

    ASSERT_TRUE(eventually([&] { return !failure_events.empty(); }));
    EXPECT_EQ(failure_events.front().generation, dependencies.generation->current());
    EXPECT_EQ(failure_events.front().error.operation, AudioOutputBackendOperation::Submit);

    const auto reconfigure_while_failed = output->configure({});
    ASSERT_FALSE(reconfigure_while_failed.has_value());
    EXPECT_EQ(reconfigure_while_failed.error().code, AudioOutputErrorCode::InvalidState);

    output->unconfigure();
    const auto reconfigured = output->configure({});
    ASSERT_TRUE(reconfigured.has_value());
}

} // namespace
} // namespace semi::domain
