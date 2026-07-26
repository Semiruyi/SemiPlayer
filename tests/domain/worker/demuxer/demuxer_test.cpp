#include "domain/worker/demuxer/default_demuxer.hpp"

#include "domain/resource/audio_packet_queue/audio_packet_queue.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace semi::domain {
namespace {

class FakeBackend final : public DemuxerBackend {
public:
    std::expected<BackendProbeResult, DemuxerBackendError> open(std::string_view) override {
        ++open_calls;
        return result;
    }

    std::expected<contracts::demuxer::BackendReadResult, DemuxerBackendError>
    read_packet() override {
        ++read_calls;
        if (!packets.empty()) {
            auto packet = std::move(packets.front());
            packets.pop_front();
            return packet;
        }
        return contracts::demuxer::BackendEndOfStream{};
    }

    void close() noexcept override {
        ++close_calls;
    }

    std::expected<BackendProbeResult, DemuxerBackendError> result;
    std::deque<contracts::demuxer::BackendReadResult> packets;
    int open_calls = 0;
    int close_calls = 0;
    std::atomic_int read_calls{0};
};

class TestEncodedPacket final : public contracts::demuxer::packet::EncodedPacket {
public:
    explicit TestEncodedPacket(std::uint8_t marker) : marker_(marker) {
        payload_[0] = std::byte{marker};
    }

    [[nodiscard]] std::span<const std::byte> payload() const noexcept override {
        return payload_;
    }

    [[nodiscard]] std::optional<std::int64_t> pts_us() const noexcept override {
        return marker_;
    }

    [[nodiscard]] std::optional<std::int64_t> dts_us() const noexcept override {
        return marker_;
    }

    [[nodiscard]] std::optional<std::int64_t> duration_us() const noexcept override {
        return 1'000;
    }

private:
    std::uint8_t marker_;
    std::array<std::byte, 1> payload_{std::byte{0}};
};

contracts::demuxer::BackendReadResult packet(std::uint32_t stream_id, std::uint8_t marker) {
    return contracts::demuxer::BackendPacket{
        .stream_id = {stream_id},
        .packet = std::make_unique<TestEncodedPacket>(marker),
    };
}

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

std::uint8_t packet_marker(const AudioPacket& packet_value) {
    return std::to_integer<std::uint8_t>(packet_value.encoded().payload().front());
}

StreamDescriptor video_stream(std::uint32_t id, std::uint32_t width) {
    return StreamDescriptor{
        .id = {id},
        .timing = {},
        .config = VideoCodecConfig{.common = {}, .coded_width = width, .coded_height = 1080,
                                   .profile = std::nullopt, .level = std::nullopt},
    };
}

struct DemuxerDependencies {
    std::shared_ptr<infra::DefaultNotifier> notifier = std::make_shared<infra::DefaultNotifier>();
    std::shared_ptr<AudioPacketQueue> audio_queue =
        std::make_shared<AudioPacketQueue>(notifier);
};

TEST(DefaultDemuxerTest, SelectsTheFirstStreamOfEachPlayableKind) {
    auto backend = std::make_shared<FakeBackend>();
    BackendProbeResult probe;
    probe.container.duration_us = 5000000;
    probe.streams = {
        video_stream(4, 640),
        video_stream(7, 1920),
        StreamDescriptor{.id = {2}, .timing = {},
                         .config = AudioCodecConfig{.common = {}, .sample_rate = 48000, .channels = 2}},
        StreamDescriptor{.id = {9}, .timing = {}, .config = SubtitleCodecConfig{}},
    };
    backend->result = probe;
    DemuxerDependencies dependencies;
    DefaultDemuxer demuxer(backend, dependencies.audio_queue, dependencies.notifier);

    const auto opened = demuxer.open("movie.mp4");

    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(opened->video.has_value());
    ASSERT_TRUE(opened->audio.has_value());
    ASSERT_TRUE(opened->subtitle.has_value());
    EXPECT_EQ(opened->video->id.value, 4U);
    EXPECT_EQ(opened->video->config.coded_width, 640U);
    EXPECT_EQ(opened->audio->id.value, 2U);
    EXPECT_EQ(opened->subtitle->id.value, 9U);
    EXPECT_EQ(backend->open_calls, 1);
}

TEST(DefaultDemuxerTest, BackendFailureLeavesTheDemuxerClosed) {
    auto backend = std::make_shared<FakeBackend>();
    backend->result = std::unexpected(DemuxerBackendError{.message = "cannot open source"});
    DemuxerDependencies dependencies;
    DefaultDemuxer demuxer(backend, dependencies.audio_queue, dependencies.notifier);

    const auto failed = demuxer.open("missing.mp4");

    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, DemuxerErrorCode::BackendFailure);
    ASSERT_TRUE(failed.error().backend_error.has_value());
    EXPECT_EQ(failed.error().backend_error->message, "cannot open source");
    EXPECT_EQ(backend->close_calls, 1);

    backend->result = BackendProbeResult{};
    EXPECT_TRUE(demuxer.open("movie.mp4").has_value());
}

TEST(DefaultDemuxerTest, RequiresAnOpenMediaBeforeStartingOrSeeking) {
    auto backend = std::make_shared<FakeBackend>();
    backend->result = BackendProbeResult{};
    DemuxerDependencies dependencies;
    DefaultDemuxer demuxer(backend, dependencies.audio_queue, dependencies.notifier);

    const auto start = demuxer.start();
    const auto seek = demuxer.seek(1'000'000);

    ASSERT_FALSE(start.has_value());
    EXPECT_EQ(start.error().code, DemuxerErrorCode::InvalidState);
    ASSERT_FALSE(seek.has_value());
    EXPECT_EQ(seek.error().code, DemuxerErrorCode::InvalidState);
}

TEST(DefaultDemuxerTest, SupportsStartStopAndSeekAfterOpen) {
    auto backend = std::make_shared<FakeBackend>();
    backend->result = BackendProbeResult{};
    DemuxerDependencies dependencies;
    DefaultDemuxer demuxer(backend, dependencies.audio_queue, dependencies.notifier);

    ASSERT_TRUE(demuxer.open("movie.mp4").has_value());
    EXPECT_TRUE(demuxer.start().has_value());
    EXPECT_TRUE(demuxer.seek(1'000'000).has_value());
    demuxer.stop();
    demuxer.stop();
    EXPECT_TRUE(demuxer.start().has_value());
}

TEST(DefaultDemuxerTest, ReadsSelectedAudioPacketsAndSkipsOtherStreams) {
    auto backend = std::make_shared<FakeBackend>();
    BackendProbeResult probe;
    probe.streams = {
        StreamDescriptor{.id = {7}, .timing = {},
                         .config = AudioCodecConfig{.common = {}, .sample_rate = 48000, .channels = 2}},
    };
    backend->result = probe;
    backend->packets.push_back(packet(3, 9));
    backend->packets.push_back(packet(7, 4));

    DemuxerDependencies dependencies;
    DefaultDemuxer demuxer(backend, dependencies.audio_queue, dependencies.notifier);
    ASSERT_TRUE(demuxer.open("movie.mp4").has_value());
    ASSERT_TRUE(demuxer.start().has_value());

    ASSERT_TRUE(wait_until([&dependencies] {
        return !dependencies.audio_queue->empty();
    }));
    auto audio_packet = dependencies.audio_queue->try_pop();
    ASSERT_TRUE(audio_packet.has_value());
    EXPECT_EQ(packet_marker(*audio_packet), 4U);
    EXPECT_EQ(audio_packet->generation(), 0U);
    EXPECT_GE(backend->read_calls.load(), 3);

    demuxer.stop();
}

TEST(DefaultDemuxerTest, StopWakesWorkerWaitingForAFullAudioQueue) {
    auto backend = std::make_shared<FakeBackend>();
    BackendProbeResult probe;
    probe.streams = {
        StreamDescriptor{.id = {2}, .timing = {},
                         .config = AudioCodecConfig{.common = {}, .sample_rate = 48000, .channels = 2}},
    };
    backend->result = probe;
    backend->packets.push_back(packet(2, 1));
    backend->packets.push_back(packet(2, 2));

    auto notifier = std::make_shared<infra::DefaultNotifier>();
    auto audio_queue = std::make_shared<AudioPacketQueue>(notifier, 1);
    DefaultDemuxer demuxer(backend, audio_queue, notifier);
    ASSERT_TRUE(demuxer.open("movie.mp4").has_value());
    ASSERT_TRUE(demuxer.start().has_value());

    ASSERT_TRUE(wait_until([&backend] {
        return backend->read_calls.load() >= 2;
    }));
    ASSERT_TRUE(audio_queue->full());

    demuxer.stop();

    auto first_packet = audio_queue->try_pop();
    ASSERT_TRUE(first_packet.has_value());
    EXPECT_EQ(packet_marker(*first_packet), 1U);
}

} // namespace
} // namespace semi::domain
