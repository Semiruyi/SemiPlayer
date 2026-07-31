#include "domain/worker/demuxer/default_demuxer.hpp"
#include "contracts/demuxer/demuxer_backend.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string_view>

namespace semi::domain {
namespace {

class FakeBackend final : public contracts::demuxer::DemuxerBackend {
public:
    std::expected<contracts::demuxer::BackendProbeResult,
                  contracts::demuxer::DemuxerBackendError>
    open(std::string_view) override {
        ++open_calls;
        return probe;
    }

    std::expected<contracts::demuxer::BackendReadResult,
                  contracts::demuxer::DemuxerBackendError>
    read_packet() override {
        return contracts::demuxer::BackendEndOfStream{};
    }

    void close() noexcept override { ++close_calls; }

    contracts::demuxer::BackendProbeResult probe;
    int open_calls = 0;
    int close_calls = 0;
};

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
    backend->probe.streams.push_back(StreamDescriptor{
        .id = {7},
        .timing = {},
        .config = AudioCodecConfig{.common = {}, .sample_rate = 48000, .channels = 2},
    });
    auto generation = std::make_shared<Generation>();
    DefaultDemuxer demuxer(backend, nullptr, nullptr, generation);

    const auto opened = demuxer.open("movie.mp4");

    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(opened->audio.has_value());
    EXPECT_EQ(opened->audio->id.value, 7U);
    EXPECT_EQ(generation->current(), 1U);
    EXPECT_EQ(backend->open_calls, 1);

    demuxer.close();
    EXPECT_EQ(backend->close_calls, 1);
}

} // namespace
} // namespace semi::domain
