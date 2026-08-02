#include "application/api_layer.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_decoder/audio_decoder.hpp"
#include "domain/worker/audio_output/audio_output.hpp"
#include "domain/worker/audio_output/audio_output_events.hpp"
#include "domain/worker/audio_resampler/audio_resampler.hpp"
#include "domain/worker/demuxer/demuxer.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace semi::application {
namespace {

class FakeDemuxer final : public domain::Demuxer {
public:
    bool fail_open = false;
    bool is_open = false;
    int open_calls = 0;
    int close_calls = 0;

    std::expected<domain::DemuxerOpenResult, domain::DemuxerError>
    open(std::string_view) override {
        ++open_calls;
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

    std::expected<void, domain::DemuxerError> seek(std::int64_t) override {
        return {};
    }

    void close() noexcept override {
        is_open = false;
        ++close_calls;
    }
};

class FakeAudioDecoder final : public domain::AudioDecoder {
public:
    bool fail_configure = false;
    int configure_calls = 0;
    int unconfigure_calls = 0;

    std::expected<domain::AudioDecoderConfigureResult, domain::AudioDecoderError>
    configure(const contracts::media::AudioCodecConfig&) override {
        ++configure_calls;
        if (fail_configure) {
            return std::unexpected(domain::AudioDecoderError{
                .code = domain::AudioDecoderErrorCode::BackendFailure,
                .message = "decoder configure failed",
                .backend_error = std::nullopt,
            });
        }
        return domain::AudioDecoderConfigureResult{
            .decoded_format = contracts::media::AudioPcmFormat{
                .sample_rate = 48000,
                .channels = 2,
                .sample_format = contracts::media::AudioSampleFormat::F32,
                .planar = false,
            },
        };
    }

    void unconfigure() noexcept override { ++unconfigure_calls; }
};

class FakeAudioResampler final : public domain::AudioResampler {
public:
    int configure_calls = 0;
    int unconfigure_calls = 0;
    contracts::media::AudioPcmFormat last_input_format;
    contracts::media::AudioPcmFormat last_output_format;

    std::expected<void, domain::AudioResamplerError>
    configure(const contracts::media::AudioPcmFormat& input_format,
              const contracts::media::AudioPcmFormat& output_format) override {
        ++configure_calls;
        last_input_format = input_format;
        last_output_format = output_format;
        return {};
    }

    void unconfigure() noexcept override { ++unconfigure_calls; }
};

class FakeAudioOutput final : public domain::AudioOutput {
public:
    bool fail_start_playback = false;
    int configure_calls = 0;
    int start_playback_calls = 0;
    int pause_playback_calls = 0;
    int unconfigure_calls = 0;

    std::expected<domain::AudioOutputConfigureResult, domain::AudioOutputError>
    configure(const domain::AudioOutputOptions&) override {
        ++configure_calls;
        return domain::AudioOutputConfigureResult{
            .playback_format = contracts::media::AudioPcmFormat{
                .sample_rate = 48000,
                .channels = 2,
                .sample_format = contracts::media::AudioSampleFormat::F32,
                .planar = false,
            },
        };
    }

    std::expected<void, domain::AudioOutputError> start_playback() override {
        ++start_playback_calls;
        if (fail_start_playback) {
            return std::unexpected(domain::AudioOutputError{
                .code = domain::AudioOutputErrorCode::BackendFailure,
                .message = "audio output start failed",
                .backend_error = std::nullopt,
            });
        }
        return {};
    }

    void pause_playback() noexcept override { ++pause_playback_calls; }

    void unconfigure() noexcept override { ++unconfigure_calls; }
};

struct FakePipeline {
    std::shared_ptr<FakeDemuxer> demuxer = std::make_shared<FakeDemuxer>();
    std::shared_ptr<FakeAudioDecoder> decoder = std::make_shared<FakeAudioDecoder>();
    std::shared_ptr<FakeAudioResampler> resampler = std::make_shared<FakeAudioResampler>();
    std::shared_ptr<FakeAudioOutput> output = std::make_shared<FakeAudioOutput>();
    std::shared_ptr<infra::DefaultNotifier> notifier = std::make_shared<infra::DefaultNotifier>();
    std::shared_ptr<domain::Generation> generation = std::make_shared<domain::Generation>();
};

ApiLayer make_layer(const FakePipeline& pipeline) {
    return ApiLayer(pipeline.demuxer,
                    pipeline.decoder,
                    pipeline.resampler,
                    pipeline.output,
                    pipeline.notifier,
                    pipeline.generation);
}

TEST(ApiLayerTest, OpenCompletesWithMediaInfoFromDemuxer) {
    FakePipeline pipeline;
    ApiLayer layer = make_layer(pipeline);
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
    EXPECT_EQ(pipeline.decoder->configure_calls, 1);
    EXPECT_EQ(pipeline.output->configure_calls, 1);
    EXPECT_EQ(pipeline.resampler->configure_calls, 1);
    EXPECT_EQ(pipeline.resampler->last_input_format.sample_rate, 48000U);
    EXPECT_EQ(pipeline.resampler->last_output_format.sample_rate, 48000U);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, AwaitConsumesHandle) {
    FakePipeline pipeline;
    ApiLayer layer = make_layer(pipeline);
    ASSERT_TRUE(layer.start());

    const CommandHandle handle = layer.set_volume(50);
    ASSERT_NE(handle, 0U);

    CommandResult result;
    EXPECT_EQ(layer.await(handle, result), SEMI_ERR_INTERNAL);
    EXPECT_EQ(layer.await(handle, result), SEMI_ERR_INVALID_HANDLE);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, OpenReturnsInvalidResourceForBackendFailure) {
    FakePipeline pipeline;
    auto demuxer = pipeline.demuxer;
    demuxer->fail_open = true;
    ApiLayer layer = make_layer(pipeline);
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

TEST(ApiLayerTest, RejectsMediaCommandsThatAreInvalidInIdle) {
    FakePipeline pipeline;
    auto demuxer = pipeline.demuxer;
    ApiLayer layer = make_layer(pipeline);
    ASSERT_TRUE(layer.start());

    const CommandHandle play = layer.play();
    const CommandHandle pause = layer.pause();
    const CommandHandle seek = layer.seek(1000000);
    ASSERT_NE(play, 0U);
    ASSERT_NE(pause, 0U);
    ASSERT_NE(seek, 0U);

    CommandResult result;
    EXPECT_EQ(layer.await(play, result), SEMI_ERR_INVALID_STATE);
    EXPECT_EQ(layer.await(pause, result), SEMI_ERR_INVALID_STATE);
    EXPECT_EQ(layer.await(seek, result), SEMI_ERR_INVALID_STATE);
    EXPECT_EQ(demuxer->open_calls, 0);
    EXPECT_EQ(demuxer->close_calls, 0);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, ChecksStateWhenQueuedCommandActuallyExecutes) {
    FakePipeline pipeline;
    ApiLayer layer = make_layer(pipeline);
    ASSERT_TRUE(layer.start());

    const CommandHandle open = layer.open("movie.mp4");
    const CommandHandle play = layer.play();
    ASSERT_NE(open, 0U);
    ASSERT_NE(play, 0U);

    CommandResult result;
    EXPECT_EQ(layer.await(open, result), SEMI_OK);
    EXPECT_EQ(layer.await(play, result), SEMI_OK);
    EXPECT_EQ(pipeline.output->start_playback_calls, 1);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, FailedCommandDoesNotCommitItsTargetState) {
    FakePipeline pipeline;
    pipeline.output->fail_start_playback = true;
    ApiLayer layer = make_layer(pipeline);
    ASSERT_TRUE(layer.start());

    CommandResult result;
    const CommandHandle open = layer.open("movie.mp4");
    ASSERT_NE(open, 0U);
    EXPECT_EQ(layer.await(open, result), SEMI_OK);

    const CommandHandle play = layer.play();
    ASSERT_NE(play, 0U);
    EXPECT_EQ(layer.await(play, result), SEMI_ERR_INTERNAL);
    EXPECT_EQ(pipeline.output->start_playback_calls, 1);

    const CommandHandle pause = layer.pause();
    ASSERT_NE(pause, 0U);
    EXPECT_EQ(layer.await(pause, result), SEMI_OK);
    EXPECT_EQ(pipeline.output->pause_playback_calls, 0);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, PlayAndPauseControlAudioOutput) {
    FakePipeline pipeline;
    ApiLayer layer = make_layer(pipeline);
    ASSERT_TRUE(layer.start());

    CommandResult result;
    const CommandHandle open = layer.open("movie.mp4");
    ASSERT_NE(open, 0U);
    EXPECT_EQ(layer.await(open, result), SEMI_OK);

    const CommandHandle play = layer.play();
    ASSERT_NE(play, 0U);
    EXPECT_EQ(layer.await(play, result), SEMI_OK);
    EXPECT_EQ(pipeline.output->start_playback_calls, 1);

    const CommandHandle duplicate_play = layer.play();
    ASSERT_NE(duplicate_play, 0U);
    EXPECT_EQ(layer.await(duplicate_play, result), SEMI_OK);
    EXPECT_EQ(pipeline.output->start_playback_calls, 1);

    const CommandHandle pause = layer.pause();
    ASSERT_NE(pause, 0U);
    EXPECT_EQ(layer.await(pause, result), SEMI_OK);
    EXPECT_EQ(pipeline.output->pause_playback_calls, 1);

    const CommandHandle duplicate_pause = layer.pause();
    ASSERT_NE(duplicate_pause, 0U);
    EXPECT_EQ(layer.await(duplicate_pause, result), SEMI_OK);
    EXPECT_EQ(pipeline.output->pause_playback_calls, 1);

    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, PollEventReturnsNoneWhenQueueIsEmpty) {
    FakePipeline pipeline;
    ApiLayer layer = make_layer(pipeline);
    ASSERT_TRUE(layer.start());

    PlayerEvent event{.type = PlayerEventType::PlaybackFinished};
    EXPECT_EQ(layer.poll_event(event), SEMI_OK);
    EXPECT_EQ(event.type, PlayerEventType::None);

    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, PlaybackFinishedMovesCurrentSessionToEnded) {
    FakePipeline pipeline;
    ApiLayer layer = make_layer(pipeline);
    ASSERT_TRUE(layer.start());

    CommandResult result;
    const CommandHandle open = layer.open("movie.mp4");
    ASSERT_NE(open, 0U);
    EXPECT_EQ(layer.await(open, result), SEMI_OK);

    const CommandHandle play = layer.play();
    ASSERT_NE(play, 0U);
    EXPECT_EQ(layer.await(play, result), SEMI_OK);
    EXPECT_EQ(pipeline.output->start_playback_calls, 1);

    const domain::AudioPlaybackFinished finished{
        .generation = pipeline.generation->current(),
    };
    EXPECT_TRUE(pipeline.notifier->send(finished));

    PlayerEvent event;
    EXPECT_EQ(layer.poll_event(event), SEMI_OK);
    EXPECT_EQ(event.type, PlayerEventType::PlaybackFinished);
    EXPECT_EQ(layer.poll_event(event), SEMI_OK);
    EXPECT_EQ(event.type, PlayerEventType::None);

    const CommandHandle pause = layer.pause();
    ASSERT_NE(pause, 0U);
    EXPECT_EQ(layer.await(pause, result), SEMI_OK);
    EXPECT_EQ(pipeline.output->pause_playback_calls, 0);

    const CommandHandle replay = layer.play();
    ASSERT_NE(replay, 0U);
    EXPECT_EQ(layer.await(replay, result), SEMI_ERR_INVALID_STATE);
    EXPECT_EQ(pipeline.output->start_playback_calls, 1);

    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, PlaybackFinishedIgnoresStaleGeneration) {
    FakePipeline pipeline;
    ApiLayer layer = make_layer(pipeline);
    ASSERT_TRUE(layer.start());

    CommandResult result;
    const CommandHandle open = layer.open("movie.mp4");
    ASSERT_NE(open, 0U);
    EXPECT_EQ(layer.await(open, result), SEMI_OK);

    const CommandHandle play = layer.play();
    ASSERT_NE(play, 0U);
    EXPECT_EQ(layer.await(play, result), SEMI_OK);

    const domain::AudioPlaybackFinished stale_finished{
        .generation = pipeline.generation->current() + 1,
    };
    EXPECT_TRUE(pipeline.notifier->send(stale_finished));

    PlayerEvent event;
    EXPECT_EQ(layer.poll_event(event), SEMI_OK);
    EXPECT_EQ(event.type, PlayerEventType::None);

    const CommandHandle pause = layer.pause();
    ASSERT_NE(pause, 0U);
    EXPECT_EQ(layer.await(pause, result), SEMI_OK);
    EXPECT_EQ(pipeline.output->pause_playback_calls, 1);

    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, CloseReleasesMediaAndAllowsAnotherOpen) {
    FakePipeline pipeline;
    auto demuxer = pipeline.demuxer;
    ApiLayer layer = make_layer(pipeline);
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

TEST(ApiLayerTest, OpenReplacesTheCurrentMedia) {
    FakePipeline pipeline;
    auto demuxer = pipeline.demuxer;
    ApiLayer layer = make_layer(pipeline);
    ASSERT_TRUE(layer.start());

    CommandResult result;
    const CommandHandle first_open = layer.open("first.mp4");
    ASSERT_NE(first_open, 0U);
    EXPECT_EQ(layer.await(first_open, result), SEMI_OK);

    const CommandHandle second_open = layer.open("second.mp4");
    ASSERT_NE(second_open, 0U);
    EXPECT_EQ(layer.await(second_open, result), SEMI_OK);
    EXPECT_EQ(demuxer->open_calls, 2);
    EXPECT_EQ(demuxer->close_calls, 1);
    EXPECT_TRUE(demuxer->is_open);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, FailedReplacementLeavesThePlayerIdle) {
    FakePipeline pipeline;
    auto demuxer = pipeline.demuxer;
    ApiLayer layer = make_layer(pipeline);
    ASSERT_TRUE(layer.start());

    CommandResult result;
    const CommandHandle first_open = layer.open("first.mp4");
    ASSERT_NE(first_open, 0U);
    EXPECT_EQ(layer.await(first_open, result), SEMI_OK);

    demuxer->fail_open = true;
    const CommandHandle replacement = layer.open("missing.mp4");
    ASSERT_NE(replacement, 0U);
    EXPECT_EQ(layer.await(replacement, result), SEMI_ERR_INVALID_RESOURCE);
    EXPECT_EQ(demuxer->close_calls, 1);
    EXPECT_FALSE(demuxer->is_open);

    const CommandHandle close = layer.close();
    ASSERT_NE(close, 0U);
    EXPECT_EQ(layer.await(close, result), SEMI_OK);
    EXPECT_EQ(demuxer->close_calls, 1);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, InvalidOpenDoesNotCloseTheCurrentMedia) {
    FakePipeline pipeline;
    auto demuxer = pipeline.demuxer;
    ApiLayer layer = make_layer(pipeline);
    ASSERT_TRUE(layer.start());

    CommandResult result;
    const CommandHandle open = layer.open("movie.mp4");
    ASSERT_NE(open, 0U);
    EXPECT_EQ(layer.await(open, result), SEMI_OK);

    const CommandHandle invalid_open = layer.open("");
    ASSERT_NE(invalid_open, 0U);
    EXPECT_EQ(layer.await(invalid_open, result), SEMI_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(demuxer->open_calls, 1);
    EXPECT_EQ(demuxer->close_calls, 0);
    EXPECT_TRUE(demuxer->is_open);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, CloseIsIdempotent) {
    FakePipeline pipeline;
    auto demuxer = pipeline.demuxer;
    ApiLayer layer = make_layer(pipeline);
    ASSERT_TRUE(layer.start());

    CommandResult result;
    const CommandHandle first_close = layer.close();
    ASSERT_NE(first_close, 0U);
    EXPECT_EQ(layer.await(first_close, result), SEMI_OK);

    const CommandHandle second_close = layer.close();
    ASSERT_NE(second_close, 0U);
    EXPECT_EQ(layer.await(second_close, result), SEMI_OK);
    EXPECT_EQ(demuxer->close_calls, 0);
    EXPECT_TRUE(layer.stop());
}

TEST(ApiLayerTest, StartAndStopAreIdempotent) {
    FakePipeline pipeline;
    ApiLayer layer = make_layer(pipeline);
    EXPECT_TRUE(layer.start());
    EXPECT_TRUE(layer.start());
    EXPECT_TRUE(layer.stop());
    EXPECT_TRUE(layer.stop());
}

} // namespace
} // namespace semi::application
