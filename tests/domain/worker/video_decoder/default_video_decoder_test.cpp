#include "domain/worker/video_decoder/default_video_decoder.hpp"

#include "contracts/video_decoder/video_decoder_backend.hpp"
#include "domain/resource/video_frame_store/video_frame_sink.hpp"
#include "domain/resource/video_frame_store/video_frame_store_item.hpp"
#include "domain/resource/video_packet_queue/video_packet.hpp"
#include "domain/resource/video_packet_queue/video_packet_source.hpp"
#include "domain/resource/video_packet_queue/video_packet_queue_events.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/worker/video_decoder/video_decoder_events.hpp"
#include "domain/worker/video_decoder/video_decoder.hpp"
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

using contracts::video_decoder::DecodedVideoBatch;
using contracts::video_decoder::VideoDecoderBackend;
using contracts::video_decoder::VideoDecoderBackendError;
using contracts::video_decoder::VideoDecoderBackendOperation;
using contracts::demuxer::packet::EncodedPacket;

class FakeVideoDecoderBackend final : public VideoDecoderBackend {
public:
    std::expected<void, VideoDecoderBackendError>
    configure(const contracts::media::VideoCodecConfig&) override {
        ++configure_calls;
        std::lock_guard lock(mutex_);
        if (configure_error_) {
            auto error = std::move(*configure_error_);
            configure_error_.reset();
            return std::unexpected(std::move(error));
        }
        return {};
    }

    std::expected<DecodedVideoBatch, VideoDecoderBackendError>
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

    std::expected<DecodedVideoBatch, VideoDecoderBackendError> drain() override {
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

    void set_configure_error(VideoDecoderBackendError error) {
        std::lock_guard lock(mutex_);
        configure_error_ = std::move(error);
    }

    void set_decode_output(DecodedVideoBatch output) {
        std::lock_guard lock(mutex_);
        decode_output_ = std::move(output);
    }

    void set_drain_output(DecodedVideoBatch output) {
        std::lock_guard lock(mutex_);
        drain_output_ = std::move(output);
    }

    void set_decode_error(VideoDecoderBackendError error) {
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
    std::optional<VideoDecoderBackendError> configure_error_;
    std::optional<VideoDecoderBackendError> decode_error_;
    std::optional<VideoDecoderBackendError> drain_error_;
    DecodedVideoBatch decode_output_;
    DecodedVideoBatch drain_output_;
};

class ThrowingVideoDecoderBackend final : public VideoDecoderBackend {
public:
    std::expected<void, VideoDecoderBackendError>
    configure(const contracts::media::VideoCodecConfig&) override {
        throw std::runtime_error("boom");
    }

    std::expected<DecodedVideoBatch, VideoDecoderBackendError>
    decode(const EncodedPacket&) override {
        return DecodedVideoBatch{};
    }

    std::expected<DecodedVideoBatch, VideoDecoderBackendError> drain() override {
        return DecodedVideoBatch{};
    }

    void reset() noexcept override {}

    void unconfigure() noexcept override { ++unconfigure_calls; }

    std::atomic_int unconfigure_calls = 0;
};

class FakeVideoPacketSource final : public VideoPacketSource {
public:
    std::optional<VideoPacketQueueItem> try_pop() override {
        ++pop_calls;
        std::lock_guard lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<VideoPacketQueueItem> item;
        item.emplace(std::move(items_.front()));
        items_.pop_front();
        return item;
    }

    void push(VideoPacketQueueItem item) {
        std::lock_guard lock(mutex_);
        items_.push_back(std::move(item));
    }

    std::atomic_int pop_calls = 0;

private:
    std::mutex mutex_;
    std::deque<VideoPacketQueueItem> items_;
};

class FakeVideoFrameSink final : public VideoFrameSink {
public:
    VideoFramePushResult try_push(VideoFrameStoreItem&& item) override {
        ++push_calls;
        std::lock_guard lock(mutex_);
        if (full_) {
            return VideoFramePushResult::Full;
        }
        items_.push_back(std::move(item));
        return VideoFramePushResult::Accepted;
    }

    void set_full(bool full) {
        std::lock_guard lock(mutex_);
        full_ = full;
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    std::optional<VideoFrameStoreItem> pop() {
        std::lock_guard lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<VideoFrameStoreItem> item;
        item.emplace(std::move(items_.front()));
        items_.pop_front();
        return item;
    }

    std::atomic_int push_calls = 0;

private:
    mutable std::mutex mutex_;
    bool full_ = false;
    std::deque<VideoFrameStoreItem> items_;
};

struct DecoderDependencies {
    std::shared_ptr<VideoPacketSource> source;
    std::shared_ptr<VideoFrameSink> sink;
    std::shared_ptr<VideoDecoderBackend> backend;
    std::shared_ptr<infra::Notifier> notifier;
    std::shared_ptr<Generation> generation;
};

std::unique_ptr<DefaultVideoDecoder> make_decoder(DecoderDependencies dependencies) {
    return std::make_unique<DefaultVideoDecoder>(std::move(dependencies.source),
                                                 std::move(dependencies.sink),
                                                 std::move(dependencies.backend),
                                                 std::move(dependencies.notifier),
                                                 std::move(dependencies.generation));
}

DecoderDependencies complete_dependencies() {
    return DecoderDependencies{
        .source = std::make_shared<FakeVideoPacketSource>(),
        .sink = std::make_shared<FakeVideoFrameSink>(),
        .backend = std::make_shared<FakeVideoDecoderBackend>(),
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

VideoPacketQueueItem make_packet_item(std::uint8_t marker, Generation::Value generation) {
    return VideoPacketQueueItem{
        std::in_place_type<VideoPacket>,
        make_encoded_packet(marker),
        generation,
    };
}

contracts::media::DecodedVideo make_decoded_video(std::uint8_t marker) {
    return contracts::media::DecodedVideo{
        .width = 2,
        .height = 1,
        .pixel_format = contracts::media::VideoPixelFormat::Rgba8,
        .planes = {contracts::media::VideoPlane{
            .bytes = {std::byte{marker}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}},
            .stride_bytes = 8,
        }},
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

TEST(DefaultVideoDecoderTest, OwnsItsWorkerAcrossSessionChanges) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeVideoDecoderBackend>(dependencies.backend);
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_TRUE(configured.has_value());
    EXPECT_EQ(backend->configure_calls, 1);

    decoder->unconfigure();
    EXPECT_EQ(backend->unconfigure_calls, 1);

    const auto reconfigured = decoder->configure({});
    ASSERT_TRUE(reconfigured.has_value());
    EXPECT_EQ(backend->configure_calls, 2);
}

TEST(DefaultVideoDecoderTest, RejectsConfigureWhileSessionIsActive) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeVideoDecoderBackend>(dependencies.backend);
    auto decoder = make_decoder(std::move(dependencies));

    ASSERT_TRUE(decoder->configure({}).has_value());

    const auto second = decoder->configure({});
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, VideoDecoderErrorCode::InvalidState);
    EXPECT_EQ(backend->configure_calls, 1);
}

TEST(DefaultVideoDecoderTest, UnconfigureIsIdempotentBeforeAnySession) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeVideoDecoderBackend>(dependencies.backend);
    auto decoder = make_decoder(std::move(dependencies));

    decoder->unconfigure();
    decoder->unconfigure();
    EXPECT_EQ(backend->unconfigure_calls, 0);
}

TEST(DefaultVideoDecoderTest, ReportsBackendConfigureFailureAndRecovers) {
    auto dependencies = complete_dependencies();
    auto backend = std::static_pointer_cast<FakeVideoDecoderBackend>(dependencies.backend);
    backend->set_configure_error(VideoDecoderBackendError{
        .operation = VideoDecoderBackendOperation::Configure,
        .native_code = -22,
        .message = "invalid codec",
    });
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, VideoDecoderErrorCode::BackendFailure);
    ASSERT_TRUE(configured.error().backend_error.has_value());
    EXPECT_EQ(configured.error().backend_error->native_code, -22);
    EXPECT_EQ(backend->unconfigure_calls, 1);

    const auto retried = decoder->configure({});
    EXPECT_TRUE(retried.has_value());
}

TEST(DefaultVideoDecoderTest, MapsBackendConfigureExceptionToFailure) {
    auto dependencies = complete_dependencies();
    dependencies.backend = std::make_shared<ThrowingVideoDecoderBackend>();
    auto backend = std::static_pointer_cast<ThrowingVideoDecoderBackend>(dependencies.backend);
    auto decoder = make_decoder(std::move(dependencies));

    const auto configured = decoder->configure({});
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, VideoDecoderErrorCode::BackendFailure);
    EXPECT_EQ(backend->unconfigure_calls, 1);
}

TEST(DefaultVideoDecoderTest, RejectsConfigureWhenDependenciesAreMissing) {
    auto decoder = make_decoder(DecoderDependencies{});

    const auto configured = decoder->configure({});
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error().code, VideoDecoderErrorCode::InvalidState);
    EXPECT_FALSE(configured.error().backend_error.has_value());

    decoder->unconfigure();
}

TEST(DefaultVideoDecoderTest, JoinsItsWorkerWhenDestroyedWhileConfigured) {
    auto dependencies = complete_dependencies();
    auto decoder = make_decoder(std::move(dependencies));

    ASSERT_TRUE(decoder->configure({}).has_value());
    decoder.reset();
}

TEST(DefaultVideoDecoderTest, DecodesVideoPacketsIntoFrameSink) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeVideoPacketSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeVideoFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeVideoDecoderBackend>(dependencies.backend);
    backend->set_decode_output(DecodedVideoBatch{make_decoded_video(1)});
    source->push(make_packet_item(1, dependencies.generation->current()));
    auto decoder = make_decoder(std::move(dependencies));

    ASSERT_TRUE(decoder->configure({}).has_value());
    ASSERT_TRUE(eventually([&sink] { return sink->size() == 1; }));
    EXPECT_EQ(backend->decode_calls, 1);

    auto item = sink->pop();
    ASSERT_TRUE(item.has_value());
    const auto* frame = std::get_if<VideoFrame>(&*item);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->generation(), 0);
    EXPECT_EQ(frame->decoded().width, 2U);
    EXPECT_EQ(frame->decoded().height, 1U);
}

TEST(DefaultVideoDecoderTest, ContinuesReadingAlreadyQueuedPacketsAfterPushingPendingOutput) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeVideoPacketSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeVideoFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeVideoDecoderBackend>(dependencies.backend);
    backend->set_decode_output(DecodedVideoBatch{make_decoded_video(1)});
    source->push(make_packet_item(1, dependencies.generation->current()));
    source->push(make_packet_item(2, dependencies.generation->current()));
    auto decoder = make_decoder(std::move(dependencies));

    ASSERT_TRUE(decoder->configure({}).has_value());
    ASSERT_TRUE(eventually([&sink] { return sink->size() == 2; }));
    EXPECT_EQ(backend->decode_calls, 2);
}

TEST(DefaultVideoDecoderTest, DrainsAndPublishesEndOfInput) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeVideoPacketSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeVideoFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeVideoDecoderBackend>(dependencies.backend);
    backend->set_drain_output(DecodedVideoBatch{make_decoded_video(2)});
    source->push(VideoPacketEndOfInput{.generation = dependencies.generation->current()});
    auto decoder = make_decoder(std::move(dependencies));

    ASSERT_TRUE(decoder->configure({}).has_value());
    ASSERT_TRUE(eventually([&sink] { return sink->size() == 2; }));
    EXPECT_EQ(backend->drain_calls, 1);

    auto frame_item = sink->pop();
    ASSERT_TRUE(frame_item.has_value());
    EXPECT_NE(std::get_if<VideoFrame>(&*frame_item), nullptr);
    auto end_item = sink->pop();
    ASSERT_TRUE(end_item.has_value());
    const auto* end = std::get_if<VideoFrameEndOfInput>(&*end_item);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end->generation, 0);
}

TEST(DefaultVideoDecoderTest, WaitsForOutputNotFullBeforePushingPendingFrame) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeVideoPacketSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeVideoFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeVideoDecoderBackend>(dependencies.backend);
    auto notifier = dependencies.notifier;
    backend->set_decode_output(DecodedVideoBatch{make_decoded_video(1)});
    sink->set_full(true);
    source->push(make_packet_item(1, dependencies.generation->current()));
    auto decoder = make_decoder(std::move(dependencies));

    ASSERT_TRUE(decoder->configure({}).has_value());
    ASSERT_TRUE(eventually([&backend] { return backend->decode_calls.load() == 1; }));
    ASSERT_TRUE(eventually([&sink] { return sink->push_calls.load() == 1; }));
    EXPECT_EQ(sink->size(), 0);

    sink->set_full(false);
    ASSERT_TRUE(notifier->send(VideoFrameStoreNotFull{}));
    ASSERT_TRUE(eventually([&sink] { return sink->size() == 1; }));
    EXPECT_GE(sink->push_calls.load(), 2);
}

TEST(DefaultVideoDecoderTest, ResetsBackendAndDropsStalePacketsWhenGenerationChanges) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeVideoPacketSource>(dependencies.source);
    auto sink = std::static_pointer_cast<FakeVideoFrameSink>(dependencies.sink);
    auto backend = std::static_pointer_cast<FakeVideoDecoderBackend>(dependencies.backend);
    auto generation = dependencies.generation;
    auto notifier = dependencies.notifier;
    backend->set_decode_output(DecodedVideoBatch{make_decoded_video(1)});
    auto decoder = make_decoder(std::move(dependencies));

    ASSERT_TRUE(decoder->configure({}).has_value());
    ASSERT_TRUE(eventually([&source] { return source->pop_calls.load() > 0; }));

    generation->bump();
    source->push(make_packet_item(1, 0));
    source->push(make_packet_item(2, generation->current()));
    ASSERT_TRUE(notifier->send(VideoQueueNotEmpty{}));

    ASSERT_TRUE(eventually([&sink] { return sink->size() == 1; }));
    EXPECT_EQ(backend->reset_calls, 1);
    EXPECT_EQ(backend->decode_calls, 1);
    auto item = sink->pop();
    ASSERT_TRUE(item.has_value());
    const auto* frame = std::get_if<VideoFrame>(&*item);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->generation(), generation->current());
}

TEST(DefaultVideoDecoderTest, ReportsDecodeFailureAndRequiresUnconfigureForRecovery) {
    auto dependencies = complete_dependencies();
    auto source = std::static_pointer_cast<FakeVideoPacketSource>(dependencies.source);
    auto backend = std::static_pointer_cast<FakeVideoDecoderBackend>(dependencies.backend);
    auto notifier = dependencies.notifier;
    std::atomic_int failure_events = 0;
    auto failure_subscription = notifier->subscribe<VideoDecoderBackendFailure>(
        [&failure_events](const VideoDecoderBackendFailure&) {
            ++failure_events;
        });
    backend->set_decode_error(VideoDecoderBackendError{
        .operation = VideoDecoderBackendOperation::Decode,
        .native_code = -1,
        .message = "decode failed",
    });
    source->push(make_packet_item(1, dependencies.generation->current()));
    auto decoder = make_decoder(std::move(dependencies));

    ASSERT_TRUE(decoder->configure({}).has_value());
    ASSERT_TRUE(eventually([&failure_events] { return failure_events.load() == 1; }));

    const auto reconfigure_while_failed = decoder->configure({});
    ASSERT_FALSE(reconfigure_while_failed.has_value());
    EXPECT_EQ(reconfigure_while_failed.error().code, VideoDecoderErrorCode::InvalidState);

    decoder->unconfigure();
    const auto reconfigured = decoder->configure({});
    EXPECT_TRUE(reconfigured.has_value());
}

} // namespace
} // namespace semi::domain
