#include "domain/worker/demuxer/default_demuxer.hpp"
#include "contracts/demuxer/demuxer_backend.hpp"
#include "domain/resource/audio_packet_queue/audio_packet_queue.hpp"
#include "domain/worker/demuxer/demuxer_events.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace semi::domain {
namespace {

using contracts::demuxer::BackendEndOfStream;
using contracts::demuxer::BackendPacket;
using contracts::demuxer::BackendProbeResult;
using contracts::demuxer::BackendReadResult;
using contracts::demuxer::DemuxerBackendError;
using contracts::demuxer::DemuxerBackendOperation;
using contracts::demuxer::packet::EncodedPacket;

using BackendReadExpected = std::expected<BackendReadResult, DemuxerBackendError>;

class FakeBackend final : public contracts::demuxer::DemuxerBackend {
public:
    std::expected<BackendProbeResult, DemuxerBackendError>
    open(std::string_view) override {
        ++open_calls;
        return probe;
    }

    std::expected<BackendReadResult, DemuxerBackendError>
    read_packet() override {
        ++read_calls;
        std::lock_guard lock(mutex_);
        if (read_results_.empty()) {
            return BackendEndOfStream{};
        }
        auto result = std::move(read_results_.front());
        read_results_.pop_front();
        return result;
    }

    void close() noexcept override { ++close_calls; }

    void push_read_result(BackendReadResult result) {
        std::lock_guard lock(mutex_);
        read_results_.emplace_back(std::move(result));
    }

    void push_read_error(DemuxerBackendError error) {
        std::lock_guard lock(mutex_);
        read_results_.emplace_back(std::unexpected(std::move(error)));
    }

    contracts::demuxer::BackendProbeResult probe;
    std::atomic_int open_calls = 0;
    std::atomic_int close_calls = 0;
    std::atomic_int read_calls = 0;

private:
    std::mutex mutex_;
    std::deque<BackendReadExpected> read_results_;
};

StreamDescriptor audio_stream(std::uint32_t id) {
    return StreamDescriptor{
        .id = {id},
        .timing = {},
        .config = AudioCodecConfig{.common = {}, .sample_rate = 48000, .channels = 2},
    };
}

StreamDescriptor video_stream(std::uint32_t id) {
    return StreamDescriptor{
        .id = {id},
        .timing = {},
        .config = VideoCodecConfig{
            .common = {},
            .coded_width = 1920,
            .coded_height = 1080,
            .profile = std::nullopt,
            .level = std::nullopt,
        },
    };
}

BackendPacket backend_packet(std::uint32_t stream_id, std::uint8_t marker) {
    EncodedPacket encoded;
    encoded.payload.push_back(std::byte{marker});
    encoded.pts_us = marker;
    return BackendPacket{
        .stream_id = {stream_id},
        .packet = std::move(encoded),
    };
}

std::optional<std::uint8_t> audio_packet_marker(const AudioPacketQueueItem& item) {
    const auto* packet = std::get_if<AudioPacket>(&item);
    if (!packet || packet->encoded().payload.empty()) {
        return std::nullopt;
    }
    return std::to_integer<std::uint8_t>(packet->encoded().payload.front());
}

template <typename Predicate>
bool wait_until(Predicate predicate) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

TEST(DefaultDemuxerTest, OwnsAndStopsItsWorkerWithTheModuleLifetime) {
    auto demuxer = std::make_unique<DefaultDemuxer>(nullptr, nullptr, nullptr, nullptr);

    demuxer->close();
    demuxer->close();
}

TEST(DefaultDemuxerTest, CompletesControlCommandsThroughTheWorker) {
    DefaultDemuxer demuxer(nullptr, nullptr, nullptr, nullptr);

    const auto opened = demuxer.open("movie.mp4");
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().code, DemuxerErrorCode::InvalidState);

    const auto seek = demuxer.seek(1'000'000);
    ASSERT_FALSE(seek.has_value());
    EXPECT_EQ(seek.error().code, DemuxerErrorCode::InvalidState);

    demuxer.close();
}

TEST(DefaultDemuxerTest, OpensAndClosesBackendThroughTheWorker) {
    auto backend = std::make_shared<FakeBackend>();
    backend->probe.streams.push_back(audio_stream(7));
    auto generation = std::make_shared<Generation>();
    auto queue = std::make_shared<AudioPacketQueue>(nullptr);
    DefaultDemuxer demuxer(backend, queue, nullptr, generation);

    const auto opened = demuxer.open("movie.mp4");

    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(opened->audio.has_value());
    EXPECT_EQ(opened->audio->id.value, 7U);
    EXPECT_EQ(generation->current(), 1U);
    EXPECT_EQ(backend->open_calls, 1);

    demuxer.close();
    EXPECT_EQ(backend->close_calls, 1);
}

TEST(DefaultDemuxerTest, ReadsSelectedAudioPacketsAndSkipsOtherStreams) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    auto backend = std::make_shared<FakeBackend>();
    backend->probe.streams.push_back(video_stream(3));
    backend->probe.streams.push_back(audio_stream(7));
    backend->push_read_result(backend_packet(3, 1));
    backend->push_read_result(backend_packet(7, 42));
    backend->push_read_result(BackendEndOfStream{});
    auto generation = std::make_shared<Generation>();
    auto queue = std::make_shared<AudioPacketQueue>(notifier, 4);
    DefaultDemuxer demuxer(backend, queue, notifier, generation);

    const auto opened = demuxer.open("movie.mp4");

    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(wait_until([&queue] { return queue->size() == 2; }));
    auto packet = queue->try_pop();
    ASSERT_TRUE(packet.has_value());
    EXPECT_EQ(audio_packet_marker(*packet), 42);
    EXPECT_EQ(audio_packet_queue_item_generation(*packet), 1U);

    auto end = queue->try_pop();
    ASSERT_TRUE(end.has_value());
    const auto* end_of_input = std::get_if<AudioPacketEndOfInput>(&*end);
    ASSERT_NE(end_of_input, nullptr);
    EXPECT_EQ(end_of_input->generation, 1U);

    demuxer.close();
}

TEST(DefaultDemuxerTest, QueuesEndOfInputWhenAudioHasNoPackets) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    auto backend = std::make_shared<FakeBackend>();
    backend->probe.streams.push_back(audio_stream(7));
    backend->push_read_result(BackendEndOfStream{});
    auto generation = std::make_shared<Generation>();
    auto queue = std::make_shared<AudioPacketQueue>(notifier, 4);
    DefaultDemuxer demuxer(backend, queue, notifier, generation);

    const auto opened = demuxer.open("movie.mp4");

    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(wait_until([&queue] { return queue->size() == 1; }));
    auto end = queue->try_pop();
    ASSERT_TRUE(end.has_value());
    const auto* end_of_input = std::get_if<AudioPacketEndOfInput>(&*end);
    ASSERT_NE(end_of_input, nullptr);
    EXPECT_EQ(end_of_input->generation, 1U);

    demuxer.close();
}

TEST(DefaultDemuxerTest, RetriesEndOfInputAfterAudioQueueBackpressure) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    auto backend = std::make_shared<FakeBackend>();
    backend->probe.streams.push_back(audio_stream(7));
    backend->push_read_result(backend_packet(7, 11));
    backend->push_read_result(BackendEndOfStream{});
    auto generation = std::make_shared<Generation>();
    auto queue = std::make_shared<AudioPacketQueue>(notifier, 1);
    DefaultDemuxer demuxer(backend, queue, notifier, generation);

    const auto opened = demuxer.open("movie.mp4");

    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(wait_until([&backend] { return backend->read_calls.load() >= 2; }));
    ASSERT_EQ(queue->size(), 1U);
    auto packet = queue->try_pop();
    ASSERT_TRUE(packet.has_value());
    EXPECT_EQ(audio_packet_marker(*packet), 11);

    ASSERT_TRUE(wait_until([&queue] { return queue->size() == 1; }));
    auto end = queue->try_pop();
    ASSERT_TRUE(end.has_value());
    ASSERT_NE(std::get_if<AudioPacketEndOfInput>(&*end), nullptr);

    demuxer.close();
}

TEST(DefaultDemuxerTest, CloseWakesWorkerWaitingForAFullAudioQueue) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    auto backend = std::make_shared<FakeBackend>();
    backend->probe.streams.push_back(audio_stream(7));
    backend->push_read_result(backend_packet(7, 1));
    backend->push_read_result(backend_packet(7, 2));
    auto generation = std::make_shared<Generation>();
    auto queue = std::make_shared<AudioPacketQueue>(notifier, 1);
    DefaultDemuxer demuxer(backend, queue, notifier, generation);

    const auto opened = demuxer.open("movie.mp4");

    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(wait_until([&backend] { return backend->read_calls.load() >= 2; }));
    demuxer.close();
    EXPECT_EQ(backend->close_calls, 1);
}

TEST(DefaultDemuxerTest, ReadFailureNotifiesAndRequiresCloseBeforeReopen) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<DemuxerBackendError> read_error;
    auto subscription = notifier->subscribe<DemuxerReadError>(
        [&mutex, &cv, &read_error](const DemuxerReadError& event) {
            {
                std::lock_guard lock(mutex);
                read_error = event.error;
            }
            cv.notify_one();
        });

    auto backend = std::make_shared<FakeBackend>();
    backend->probe.streams.push_back(audio_stream(7));
    backend->push_read_error(DemuxerBackendError{
        .operation = DemuxerBackendOperation::Read,
        .native_code = -5,
        .message = "read failed",
    });
    auto generation = std::make_shared<Generation>();
    auto queue = std::make_shared<AudioPacketQueue>(notifier, 4);
    DefaultDemuxer demuxer(backend, queue, notifier, generation);

    const auto opened = demuxer.open("movie.mp4");

    ASSERT_TRUE(opened.has_value());
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&read_error] {
            return read_error.has_value();
        }));
        EXPECT_EQ(read_error->operation, DemuxerBackendOperation::Read);
    }

    const auto reopened_without_close = demuxer.open("movie.mp4");
    ASSERT_FALSE(reopened_without_close.has_value());
    EXPECT_EQ(reopened_without_close.error().code, DemuxerErrorCode::InvalidState);

    demuxer.close();
    backend->push_read_result(BackendEndOfStream{});
    const auto reopened = demuxer.open("movie.mp4");
    EXPECT_TRUE(reopened.has_value());

    demuxer.close();
}

TEST(DefaultDemuxerTest, CloseDiscardsPendingPacketAndAllowsReopenWithNextGeneration) {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    auto backend = std::make_shared<FakeBackend>();
    backend->probe.streams.push_back(audio_stream(7));
    backend->push_read_result(backend_packet(7, 1));
    backend->push_read_result(backend_packet(7, 2));
    auto generation = std::make_shared<Generation>();
    auto queue = std::make_shared<AudioPacketQueue>(notifier, 1);
    DefaultDemuxer demuxer(backend, queue, notifier, generation);

    const auto opened = demuxer.open("movie.mp4");

    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(wait_until([&backend] { return backend->read_calls.load() >= 2; }));
    demuxer.close();

    backend->push_read_result(backend_packet(7, 3));
    const auto reopened = demuxer.open("movie.mp4");
    ASSERT_TRUE(reopened.has_value());
    EXPECT_EQ(generation->current(), 2U);

    auto stale = queue->try_pop();
    ASSERT_TRUE(stale.has_value());
    EXPECT_EQ(audio_packet_marker(*stale), 1);
    EXPECT_EQ(audio_packet_queue_item_generation(*stale), 1U);

    ASSERT_TRUE(wait_until([&queue] { return queue->size() == 1; }));
    auto current = queue->try_pop();
    ASSERT_TRUE(current.has_value());
    EXPECT_EQ(audio_packet_marker(*current), 3);
    EXPECT_EQ(audio_packet_queue_item_generation(*current), 2U);

    demuxer.close();
}

} // namespace
} // namespace semi::domain
