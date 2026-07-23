#include "application/api_layer.hpp"
#include "domain/demuxer/demuxer.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace semi::application {
namespace {

class FakeDemuxer final : public domain::Demuxer {
public:
    bool fail_open = false;
    bool is_open = false;
    int close_calls = 0;

    std::expected<domain::DemuxerOpenResult, domain::DemuxerError>
    open(std::string_view) override {
        if (is_open) {
            return std::unexpected(domain::DemuxerError{
                .code = domain::DemuxerErrorCode::InvalidState,
                .message = "demuxer is already open",
                .backend_error = std::nullopt,
            });
        }
        if (fail_open) {
            return std::unexpected(domain::DemuxerError{
                .code = domain::DemuxerErrorCode::BackendFailure,
                .message = "cannot open source",
                .backend_error = domain::DemuxerBackendError{
                    .operation = domain::DemuxerBackendOperation::Open,
                    .native_code = -2,
                    .message = "No such file or directory",
                },
            });
        }
        is_open = true;
        domain::DemuxerOpenResult result;
        result.container.duration_us = 1234567;
        result.video = domain::SelectedStream<domain::VideoCodecConfig>{
            .id = {0},
            .timing = {},
            .config = {.common = {}, .coded_width = 1920, .coded_height = 1080,
                       .profile = std::nullopt, .level = std::nullopt},
        };
        result.audio = domain::SelectedStream<domain::AudioCodecConfig>{
            .id = {1},
            .timing = {},
            .config = {.common = {}, .sample_rate = 48000, .channels = 2},
        };
        return result;
    }

    void close() noexcept override {
        is_open = false;
        ++close_calls;
    }
};

std::shared_ptr<domain::Demuxer> make_fake_demuxer() {
    return std::make_shared<FakeDemuxer>();
}

TEST(ApiLayerTest, OpenCompletesWithMediaInfoFromDemuxer) {
    ApiLayer layer(make_fake_demuxer());
    ASSERT_TRUE(layer.start());

    const CommandHandle handle = layer.open("movie.mp4");
    ASSERT_NE(handle, 0U);

    CommandResult result;
    EXPECT_EQ(layer.await(handle, result), SEMI_OK);
    EXPECT_TRUE(result.has_media_info);
    EXPECT_EQ(result.media_info.duration_us, 1234567);
    EXPECT_TRUE(result.media_info.has_video);
    EXPECT_TRUE(result.media_info.has_audio);
    EXPECT_FALSE(result.media_info.has_subtitle);
    EXPECT_EQ(result.media_info.video_width, 1920U);
    EXPECT_EQ(result.media_info.video_height, 1080U);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, AwaitConsumesHandle) {
    ApiLayer layer(make_fake_demuxer());
    ASSERT_TRUE(layer.start());

    const CommandHandle handle = layer.play();
    ASSERT_NE(handle, 0U);

    CommandResult result;
    EXPECT_EQ(layer.await(handle, result), SEMI_ERR_INTERNAL);
    EXPECT_EQ(layer.await(handle, result), SEMI_ERR_INVALID_HANDLE);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, OpenReturnsInvalidResourceForBackendFailure) {
    auto demuxer = std::make_shared<FakeDemuxer>();
    demuxer->fail_open = true;
    ApiLayer layer(demuxer);
    ASSERT_TRUE(layer.start());

    const CommandHandle failed_handle = layer.open("missing.mp4");
    ASSERT_NE(failed_handle, 0U);
    CommandResult failed_result;
    EXPECT_EQ(layer.await(failed_handle, failed_result), SEMI_ERR_INVALID_RESOURCE);
    EXPECT_FALSE(failed_result.has_media_info);

    demuxer->fail_open = false;
    const CommandHandle retry_handle = layer.open("movie.mp4");
    ASSERT_NE(retry_handle, 0U);
    CommandResult retry_result;
    EXPECT_EQ(layer.await(retry_handle, retry_result), SEMI_OK);
    EXPECT_TRUE(retry_result.has_media_info);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, CloseReleasesMediaAndAllowsAnotherOpen) {
    auto demuxer = std::make_shared<FakeDemuxer>();
    ApiLayer layer(demuxer);
    ASSERT_TRUE(layer.start());

    CommandResult result;
    const CommandHandle first_open = layer.open("first.mp4");
    ASSERT_NE(first_open, 0U);
    EXPECT_EQ(layer.await(first_open, result), SEMI_OK);

    const CommandHandle close = layer.close();
    ASSERT_NE(close, 0U);
    result = {};
    EXPECT_EQ(layer.await(close, result), SEMI_OK);
    EXPECT_FALSE(result.has_media_info);
    EXPECT_EQ(demuxer->close_calls, 1);

    const CommandHandle second_open = layer.open("second.mp4");
    ASSERT_NE(second_open, 0U);
    EXPECT_EQ(layer.await(second_open, result), SEMI_OK);
    EXPECT_TRUE(result.has_media_info);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, CloseIsIdempotent) {
    auto demuxer = std::make_shared<FakeDemuxer>();
    ApiLayer layer(demuxer);
    ASSERT_TRUE(layer.start());

    CommandResult result;
    const CommandHandle first_close = layer.close();
    ASSERT_NE(first_close, 0U);
    EXPECT_EQ(layer.await(first_close, result), SEMI_OK);

    const CommandHandle second_close = layer.close();
    ASSERT_NE(second_close, 0U);
    EXPECT_EQ(layer.await(second_close, result), SEMI_OK);
    EXPECT_EQ(demuxer->close_calls, 2);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, StartAndStopAreIdempotent) {
    ApiLayer layer(make_fake_demuxer());
    EXPECT_TRUE(layer.start());
    EXPECT_TRUE(layer.start());
    EXPECT_TRUE(layer.stop());
    EXPECT_TRUE(layer.stop());
}

} // namespace
} // namespace semi::application
