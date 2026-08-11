#include "domain/worker/video_renderer/default_video_renderer.hpp"

#include "domain/resource/generation/generation.hpp"
#include "domain/resource/video_frame_store/video_frame_store_item.hpp"
#include "domain/resource/video_frame_store/video_frame_source.hpp"
#include "domain/resource/video_rendered_store/video_rendered_store_item.hpp"
#include "domain/resource/video_rendered_store/video_rendered_store_sink.hpp"
#include "domain/worker/video_renderer/video_renderer_events.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <gtest/gtest.h>

#include <array>
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
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace semi::domain {
namespace {

using contracts::video_renderer::VideoRendererBackend;
using contracts::video_renderer::VideoRendererBackendError;
using contracts::video_renderer::VideoRendererBackendOperation;

class TestVideoFrameBuffer final : public contracts::media::VideoFrameBuffer {
public:
    explicit TestVideoFrameBuffer(std::uint8_t marker)
        : bytes_{std::byte{marker}, std::byte{0}, std::byte{0}, std::byte{255}} {}

    [[nodiscard]] contracts::media::VideoPixelFormat pixel_format() const noexcept override {
        return contracts::media::VideoPixelFormat::Rgba8;
    }

    [[nodiscard]] std::uint32_t width() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t height() const noexcept override { return 1; }
    [[nodiscard]] std::size_t plane_count() const noexcept override { return 1; }

    [[nodiscard]] contracts::media::VideoPlaneView
    plane(std::size_t index) const noexcept override {
        if (index != 0) {
            return {};
        }
        return contracts::media::VideoPlaneView{
            .data = bytes_.data(),
            .size_bytes = bytes_.size(),
            .stride_bytes = 4,
        };
    }

private:
    std::array<std::byte, 4> bytes_;
};

VideoFrame make_video_frame(std::uint8_t marker, Generation::Value generation) {
    contracts::media::DecodedVideo decoded{
        .buffer = std::make_unique<TestVideoFrameBuffer>(marker),
        .pts_us = static_cast<std::int64_t>(marker),
    };
    return VideoFrame(std::move(decoded), generation);
}

std::uint8_t input_marker(const contracts::media::DecodedVideo& decoded) {
    return std::to_integer<std::uint8_t>(decoded.buffer->plane(0).bytes().front());
}

class FakeVideoRendererBackend final : public VideoRendererBackend {
public:
    std::expected<void, VideoRendererBackendError>
    configure(const contracts::video_renderer::VideoRendererOptions&) override {
        ++configure_calls;
        std::lock_guard lock(mutex_);
        if (configure_error_) {
            auto error = std::move(*configure_error_);
            configure_error_.reset();
            return std::unexpected(std::move(error));
        }
        return {};
    }

    std::expected<contracts::media::RenderedVideo, VideoRendererBackendError>
    render(const contracts::media::DecodedVideo& input) override {
        ++render_calls;
        std::lock_guard lock(mutex_);
        if (render_error_) {
            auto error = std::move(*render_error_);
            render_error_.reset();
            return std::unexpected(std::move(error));
        }

        const auto marker = input_marker(input);
        return contracts::media::RenderedVideo{
            .pixel_format = contracts::media::VideoPixelFormat::Rgba8,
            .width = 1,
            .height = 1,
            .stride_bytes = 4,
            .pixels = {std::byte{marker}, std::byte{1}, std::byte{2}, std::byte{255}},
            .pts_us = input.pts_us,
        };
    }

    void reset() noexcept override { ++reset_calls; }
    void unconfigure() noexcept override { ++unconfigure_calls; }

    void set_configure_error(VideoRendererBackendError error) {
        std::lock_guard lock(mutex_);
        configure_error_ = std::move(error);
    }

    void set_render_error(VideoRendererBackendError error) {
        std::lock_guard lock(mutex_);
        render_error_ = std::move(error);
    }

    std::atomic_int configure_calls = 0;
    std::atomic_int render_calls = 0;
    std::atomic_int reset_calls = 0;
    std::atomic_int unconfigure_calls = 0;

private:
    std::mutex mutex_;
    std::optional<VideoRendererBackendError> configure_error_;
    std::optional<VideoRendererBackendError> render_error_;
};

class ThrowingVideoRendererBackend final : public VideoRendererBackend {
public:
    std::expected<void, VideoRendererBackendError>
    configure(const contracts::video_renderer::VideoRendererOptions&) override {
        throw std::runtime_error("boom");
    }

    std::expected<contracts::media::RenderedVideo, VideoRendererBackendError>
    render(const contracts::media::DecodedVideo&) override {
        return contracts::media::RenderedVideo{};
    }

    void reset() noexcept override {}
    void unconfigure() noexcept override { ++unconfigure_calls; }

    std::atomic_int unconfigure_calls = 0;
};

class FakeVideoFrameSource final : public VideoFrameSource {
public:
    std::optional<VideoFrameStoreItem> try_pop() override {
        ++pop_calls;
        std::lock_guard lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<VideoFrameStoreItem> item;
        item.emplace(std::move(items_.front()));
        items_.pop_front();
        return item;
    }

    void push(VideoFrameStoreItem item) {
        std::lock_guard lock(mutex_);
        items_.push_back(std::move(item));
    }

    std::atomic_int pop_calls = 0;

private:
    std::mutex mutex_;
    std::deque<VideoFrameStoreItem> items_;
};

class FakeVideoRenderedSink final : public VideoRenderedSink {
public:
    VideoRenderedPushResult try_push(VideoRenderedStoreItem&& item) override {
        ++push_calls;
        std::lock_guard lock(mutex_);
        if (full_) {
            return VideoRenderedPushResult::Full;
        }
        items_.push_back(std::move(item));
        return VideoRenderedPushResult::Accepted;
    }

    void set_full(bool full) {
        std::lock_guard lock(mutex_);
        full_ = full;
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    std::optional<VideoRenderedStoreItem> pop() {
        std::lock_guard lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<VideoRenderedStoreItem> item;
        item.emplace(std::move(items_.front()));
        items_.pop_front();
        return item;
    }

    std::atomic_int push_calls = 0;

private:
    mutable std::mutex mutex_;
    bool full_ = false;
    std::deque<VideoRenderedStoreItem> items_;
};

struct RendererDependencies {
    std::shared_ptr<VideoFrameSource> source;
    std::shared_ptr<VideoRenderedSink> sink;
    std::shared_ptr<VideoRendererBackend> backend;
    std::shared_ptr<infra::Notifier> notifier;
    std::shared_ptr<Generation> generation;
};

std::unique_ptr<DefaultVideoRenderer>
make_renderer(RendererDependencies dependencies) {
    return std::make_unique<DefaultVideoRenderer>(std::move(dependencies.source),
                                                   std::move(dependencies.sink),
                                                   std::move(dependencies.backend),
                                                   std::move(dependencies.notifier),
                                                   std::move(dependencies.generation));
}

RendererDependencies complete_dependencies() {
    return RendererDependencies{
        .source = std::make_shared<FakeVideoFrameSource>(),
        .sink = std::make_shared<FakeVideoRenderedSink>(),
        .backend = std::make_shared<FakeVideoRendererBackend>(),
        .notifier = std::make_shared<infra::DefaultNotifier>(),
        .generation = std::make_shared<Generation>(),
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

TEST(DefaultVideoRendererTest, OwnsItsWorkerAcrossSessionChanges) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeVideoRendererBackend>(dependencies.backend);
    auto renderer = make_renderer(std::move(dependencies));

    ASSERT_TRUE(renderer->configure({}).has_value());
    EXPECT_EQ(backend->configure_calls, 1);

    renderer->unconfigure();
    EXPECT_EQ(backend->unconfigure_calls, 1);

    ASSERT_TRUE(renderer->configure({}).has_value());
    EXPECT_EQ(backend->configure_calls, 2);
}

TEST(DefaultVideoRendererTest, RejectsConfigureWhileSessionIsActive) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeVideoRendererBackend>(dependencies.backend);
    auto renderer = make_renderer(std::move(dependencies));

    ASSERT_TRUE(renderer->configure({}).has_value());
    const auto second = renderer->configure({});
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, VideoRendererErrorCode::InvalidState);
    EXPECT_EQ(backend->configure_calls, 1);
}

TEST(DefaultVideoRendererTest, ReportsBackendConfigureFailureAndRecovers) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeVideoRendererBackend>(dependencies.backend);
    backend->set_configure_error(VideoRendererBackendError{
        .operation = VideoRendererBackendOperation::Render,
        .native_code = -22,
        .message = "invalid output format",
    });
    auto renderer = make_renderer(std::move(dependencies));

    const auto configured = renderer->configure({});
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, VideoRendererErrorCode::BackendFailure);
    ASSERT_TRUE(configured.error().backend_error.has_value());
    EXPECT_EQ(configured.error().backend_error->native_code, -22);
    EXPECT_EQ(backend->unconfigure_calls, 1);

    EXPECT_TRUE(renderer->configure({}).has_value());
}

TEST(DefaultVideoRendererTest, MapsBackendConfigureExceptionToFailure) {
    auto dependencies = complete_dependencies();
    dependencies.backend = std::make_shared<ThrowingVideoRendererBackend>();
    auto backend = std::static_pointer_cast<ThrowingVideoRendererBackend>(dependencies.backend);
    auto renderer = make_renderer(std::move(dependencies));

    const auto configured = renderer->configure({});
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, VideoRendererErrorCode::BackendFailure);
    ASSERT_TRUE(configured.error().backend_error.has_value());
    EXPECT_EQ(configured.error().backend_error->operation,
              VideoRendererBackendOperation::Configure);
    EXPECT_EQ(backend->unconfigure_calls, 1);
}

TEST(DefaultVideoRendererTest, RejectsConfigureWhenDependenciesAreMissing) {
    auto renderer = make_renderer(RendererDependencies{});

    const auto configured = renderer->configure({});
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, VideoRendererErrorCode::InvalidState);

    renderer->unconfigure();
}

TEST(DefaultVideoRendererTest, RendersDecodedFramesIntoRenderedSink) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeVideoFrameSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeVideoRenderedSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeVideoRendererBackend>(dependencies.backend);
    auto generation = dependencies.generation;
    source->push(make_video_frame(7, generation->current()));
    auto renderer = make_renderer(std::move(dependencies));

    ASSERT_TRUE(renderer->configure({}).has_value());
    ASSERT_TRUE(eventually([&sink] { return sink->size() == 1; }));
    EXPECT_EQ(backend->render_calls, 1);

    auto item = sink->pop();
    ASSERT_TRUE(item.has_value());
    const auto* frame = std::get_if<RenderedVideoFrame>(&*item);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->generation(), generation->current());
    EXPECT_EQ(frame->rendered().pixels.front(), std::byte{7});
    EXPECT_EQ(frame->rendered().pts_us, 7);
}

TEST(DefaultVideoRendererTest, PublishesEndOfInput) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeVideoFrameSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeVideoRenderedSink>(dependencies.sink);
    auto generation = dependencies.generation;
    source->push(VideoFrameEndOfInput{.generation = generation->current()});
    auto renderer = make_renderer(std::move(dependencies));

    ASSERT_TRUE(renderer->configure({}).has_value());
    ASSERT_TRUE(eventually([&sink] { return sink->size() == 1; }));
    auto item = sink->pop();
    ASSERT_TRUE(item.has_value());
    const auto* end = std::get_if<RenderedVideoEndOfInput>(&*item);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end->generation, generation->current());
}

TEST(DefaultVideoRendererTest, WaitsForOutputNotFullBeforePushingPendingFrame) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeVideoFrameSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeVideoRenderedSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeVideoRendererBackend>(dependencies.backend);
    auto notifier = dependencies.notifier;
    sink->set_full(true);
    source->push(make_video_frame(1, dependencies.generation->current()));
    auto renderer = make_renderer(std::move(dependencies));

    ASSERT_TRUE(renderer->configure({}).has_value());
    ASSERT_TRUE(eventually([&backend] { return backend->render_calls.load() == 1; }));
    ASSERT_TRUE(eventually([&sink] { return sink->push_calls.load() == 1; }));
    EXPECT_EQ(sink->size(), 0U);

    sink->set_full(false);
    ASSERT_TRUE(notifier->send(VideoRenderedStoreNotFull{}));
    ASSERT_TRUE(eventually([&sink] { return sink->size() == 1; }));
    EXPECT_GE(sink->push_calls.load(), 2);
}

TEST(DefaultVideoRendererTest, ResetsBackendAndDropsStaleFramesWhenGenerationChanges) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeVideoFrameSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeVideoRenderedSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeVideoRendererBackend>(dependencies.backend);
    auto generation = dependencies.generation;
    auto notifier = dependencies.notifier;
    auto renderer = make_renderer(std::move(dependencies));

    ASSERT_TRUE(renderer->configure({}).has_value());
    ASSERT_TRUE(eventually([&source] { return source->pop_calls.load() > 0; }));

    generation->bump();
    source->push(make_video_frame(1, 0));
    source->push(make_video_frame(2, generation->current()));
    ASSERT_TRUE(notifier->send(VideoFrameStoreNotEmpty{}));

    ASSERT_TRUE(eventually([&sink] { return sink->size() == 1; }));
    EXPECT_EQ(backend->reset_calls, 1);
    EXPECT_EQ(backend->render_calls, 1);
    auto item = sink->pop();
    ASSERT_TRUE(item.has_value());
    const auto* frame = std::get_if<RenderedVideoFrame>(&*item);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->rendered().pixels.front(), std::byte{2});
    EXPECT_EQ(frame->generation(), generation->current());
}

TEST(DefaultVideoRendererTest, ReportsRenderFailureAndRequiresUnconfigureForRecovery) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeVideoFrameSource>(dependencies.source);
    auto backend = std::static_pointer_cast<FakeVideoRendererBackend>(dependencies.backend);
    auto notifier = dependencies.notifier;
    std::atomic_int failure_events = 0;
    auto failure_subscription = notifier->subscribe<VideoRendererBackendFailure>(
        [&failure_events](const VideoRendererBackendFailure&) {
            ++failure_events;
        });
    backend->set_render_error(VideoRendererBackendError{
        .operation = VideoRendererBackendOperation::Render,
        .native_code = -1,
        .message = "render failed",
    });
    source->push(make_video_frame(1, dependencies.generation->current()));
    auto renderer = make_renderer(std::move(dependencies));

    ASSERT_TRUE(renderer->configure({}).has_value());
    ASSERT_TRUE(eventually([&failure_events] { return failure_events.load() == 1; }));

    const auto reconfigure_while_failed = renderer->configure({});
    ASSERT_FALSE(reconfigure_while_failed.has_value());
    EXPECT_EQ(reconfigure_while_failed.error().code, VideoRendererErrorCode::InvalidState);

    renderer->unconfigure();
    EXPECT_TRUE(renderer->configure({}).has_value());
    EXPECT_TRUE(failure_subscription->active());
}

} // namespace
} // namespace semi::domain
