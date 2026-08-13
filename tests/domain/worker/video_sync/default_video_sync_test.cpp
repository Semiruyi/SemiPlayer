#include "domain/resource/generation/generation.hpp"
#include "domain/resource/video_rendered_store/video_rendered_store.hpp"
#include "domain/worker/audio_output/audio_output.hpp"
#include "domain/worker/video_sync/default_video_sync.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace semi::domain {
namespace {

class FakeAudioOutput final : public AudioOutput {
public:
    std::expected<AudioOutputConfigureResult, AudioOutputError>
    configure(const AudioOutputOptions&) override {
        return AudioOutputConfigureResult{
            .playback_format = contracts::media::AudioPcmFormat{
                .sample_rate = 48'000,
                .channels = 2,
                .sample_format = contracts::media::AudioSampleFormat::F32,
                .planar = false,
            },
        };
    }

    std::expected<void, AudioOutputError> start_playback() override { return {}; }
    std::expected<void, AudioOutputError> pause_playback() override { return {}; }

    std::optional<PlaybackPosition> current_position() const noexcept override {
        if (!position_available_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        return PlaybackPosition{
            .generation = position_generation_.load(std::memory_order_acquire),
            .pts_us = position_pts_us_.load(std::memory_order_acquire),
        };
    }

    void unconfigure() noexcept override {}

    void set_position(Generation::Value generation, std::int64_t pts_us) noexcept {
        position_generation_.store(generation, std::memory_order_release);
        position_pts_us_.store(pts_us, std::memory_order_release);
        position_available_.store(true, std::memory_order_release);
    }

private:
    std::atomic<bool> position_available_ = false;
    std::atomic<std::uint64_t> position_generation_ = 0;
    std::atomic<std::int64_t> position_pts_us_ = 0;
};

struct PresentedFrame {
    Generation::Value generation = 0;
    std::int64_t pts_us = 0;
    std::uint8_t marker = 0;
};

class RecordingPresentation final {
public:
    RecordingPresentation() {
        frames_.reserve(8);
    }

    void present(const RenderedVideoFrame& frame) noexcept {
        const auto& rendered = frame.rendered();
        PresentedFrame presented{
            .generation = frame.generation(),
            .pts_us = rendered.pts_us.value_or(-1),
            .marker = rendered.pixels.empty()
                          ? std::uint8_t{0}
                          : std::to_integer<std::uint8_t>(rendered.pixels.front()),
        };
        {
            std::lock_guard lock(mutex_);
            frames_.push_back(presented);
        }
        cv_.notify_all();
    }

    bool wait_for_frames(std::size_t count, std::chrono::milliseconds timeout) const {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, count] {
            return frames_.size() >= count;
        });
    }

    std::vector<PresentedFrame> frames() const {
        std::lock_guard lock(mutex_);
        return frames_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::vector<PresentedFrame> frames_;
};

class BlockingPresentation final {
public:
    void present(const RenderedVideoFrame&) {
        std::unique_lock lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] {
            return may_return_;
        });
    }

    bool wait_until_entered(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] {
            return entered_;
        });
    }

    void allow_return() {
        {
            std::lock_guard lock(mutex_);
            may_return_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool may_return_ = false;
};

contracts::media::RenderedVideo make_rendered_video(std::uint8_t marker,
                                                    std::int64_t pts_us) {
    return contracts::media::RenderedVideo{
        .pixel_format = contracts::media::VideoPixelFormat::Rgba8,
        .width = 1,
        .height = 1,
        .stride_bytes = 4,
        .pixels = {std::byte{marker}, std::byte{0}, std::byte{0}, std::byte{255}},
        .pts_us = pts_us,
    };
}

VideoRenderedStoreItem make_frame(std::uint8_t marker,
                                  std::int64_t pts_us,
                                  Generation::Value generation) {
    return VideoRenderedStoreItem{
        std::in_place_type<RenderedVideoFrame>,
        make_rendered_video(marker, pts_us),
        generation,
    };
}

TEST(DefaultVideoSyncTest, AudioClockPresentsNewestDueFrameAndWaitsForFutureFrame) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    auto generation = std::make_shared<Generation>(notifier);
    auto rendered_store = std::make_shared<VideoRenderedStore>(notifier);
    auto audio_output = std::make_shared<FakeAudioOutput>();
    auto presentation = std::make_shared<RecordingPresentation>();

    ASSERT_EQ(rendered_store->try_push(make_frame(1, 100'000, generation->current())),
              VideoRenderedPushResult::Accepted);
    ASSERT_EQ(rendered_store->try_push(make_frame(2, 120'000, generation->current())),
              VideoRenderedPushResult::Accepted);
    ASSERT_EQ(rendered_store->try_push(make_frame(3, 2'000'000, generation->current())),
              VideoRenderedPushResult::Accepted);

    DefaultVideoSync sync(rendered_store, audio_output, notifier, generation);
    ASSERT_TRUE(sync.configure(VideoSyncOptions{
        .audio_master = true,
        .on_frame = [presentation](const RenderedVideoFrame& frame) {
            presentation->present(frame);
        },
    }));
    audio_output->set_position(generation->current(), 150'000);
    ASSERT_TRUE(sync.start_playback());

    ASSERT_TRUE(presentation->wait_for_frames(1, std::chrono::seconds(1)));
    auto presented = presentation->frames();
    ASSERT_EQ(presented.size(), 1U);
    EXPECT_EQ(presented.front().generation, generation->current());
    EXPECT_EQ(presented.front().pts_us, 120'000);
    EXPECT_EQ(presented.front().marker, 2);

    audio_output->set_position(generation->current(), 2'100'000);
    ASSERT_TRUE(notifier->send(AudioPlaybackPositionReady{
        .generation = generation->current(),
    }));
    ASSERT_TRUE(presentation->wait_for_frames(2, std::chrono::seconds(1)));
    presented = presentation->frames();
    ASSERT_EQ(presented.size(), 2U);
    EXPECT_EQ(presented.back().pts_us, 2'000'000);
    EXPECT_EQ(presented.back().marker, 3);

    sync.unconfigure();
}

TEST(DefaultVideoSyncTest,
     DiscardsStaleGenerationAndPresentsFuturePausedFrameAfterSeek) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    auto generation = std::make_shared<Generation>(notifier);
    auto rendered_store = std::make_shared<VideoRenderedStore>(notifier);
    auto audio_output = std::make_shared<FakeAudioOutput>();
    auto presentation = std::make_shared<RecordingPresentation>();

    ASSERT_EQ(rendered_store->try_push(make_frame(1, 10'000, generation->current())),
              VideoRenderedPushResult::Accepted);

    DefaultVideoSync sync(rendered_store, audio_output, notifier, generation);
    ASSERT_TRUE(sync.configure(VideoSyncOptions{
        .audio_master = true,
        .on_frame = [presentation](const RenderedVideoFrame& frame) {
            presentation->present(frame);
        },
    }));

    const auto new_generation = generation->bump();
    // A paused audio clock cannot advance to a slightly later video PTS. The
    // first frame from the seek generation must still be presented immediately.
    audio_output->set_position(new_generation, 50'000);
    ASSERT_EQ(rendered_store->try_push(make_frame(2, 100'000, new_generation)),
              VideoRenderedPushResult::Accepted);

    ASSERT_TRUE(presentation->wait_for_frames(1, std::chrono::seconds(1)));
    const auto presented = presentation->frames();
    ASSERT_EQ(presented.size(), 1U);
    EXPECT_EQ(presented.front().generation, new_generation);
    EXPECT_EQ(presented.front().marker, 2);
    EXPECT_EQ(presented.front().pts_us, 100'000);

    sync.unconfigure();
}

TEST(DefaultVideoSyncTest, UnconfigureWaitsForSynchronousFrameCallback) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    auto generation = std::make_shared<Generation>(notifier);
    auto rendered_store = std::make_shared<VideoRenderedStore>(notifier);
    auto presentation = std::make_shared<BlockingPresentation>();

    ASSERT_EQ(rendered_store->try_push(make_frame(1, 0, generation->current())),
              VideoRenderedPushResult::Accepted);

    DefaultVideoSync sync(rendered_store, nullptr, notifier, generation);
    ASSERT_TRUE(sync.configure(VideoSyncOptions{
        .audio_master = false,
        .on_frame = [presentation](const RenderedVideoFrame& frame) {
            presentation->present(frame);
        },
    }));
    ASSERT_TRUE(sync.start_playback());
    ASSERT_TRUE(presentation->wait_until_entered(std::chrono::seconds(1)));

    std::atomic<bool> unconfigured = false;
    std::thread unconfigure_thread([&sync, &unconfigured] {
        sync.unconfigure();
        unconfigured.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(unconfigured.load(std::memory_order_acquire));
    presentation->allow_return();
    unconfigure_thread.join();
    EXPECT_TRUE(unconfigured.load(std::memory_order_acquire));
}

} // namespace
} // namespace semi::domain
