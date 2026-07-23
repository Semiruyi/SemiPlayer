#include "domain/demuxer/demuxer.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace semi::domain {
namespace {

class FakeBackend final : public DemuxerBackend {
public:
    std::expected<BackendProbeResult, DemuxerBackendError> open(std::string_view) override {
        ++open_calls;
        return result;
    }

    void close() noexcept override {
        ++close_calls;
    }

    std::expected<BackendProbeResult, DemuxerBackendError> result;
    int open_calls = 0;
    int close_calls = 0;
};

StreamDescriptor video_stream(std::uint32_t id, std::uint32_t width) {
    return StreamDescriptor{
        .id = {id},
        .timing = {},
        .config = VideoCodecConfig{.common = {}, .coded_width = width, .coded_height = 1080,
                                   .profile = std::nullopt, .level = std::nullopt},
    };
}

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
    DefaultDemuxer demuxer(backend);

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
    DefaultDemuxer demuxer(backend);

    const auto failed = demuxer.open("missing.mp4");

    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, DemuxerErrorCode::BackendFailure);
    ASSERT_TRUE(failed.error().backend_error.has_value());
    EXPECT_EQ(failed.error().backend_error->message, "cannot open source");
    EXPECT_EQ(backend->close_calls, 1);

    backend->result = BackendProbeResult{};
    EXPECT_TRUE(demuxer.open("movie.mp4").has_value());
}

} // namespace
} // namespace semi::domain
