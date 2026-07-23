extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include "infrastructure/ffmpeg/ffmpeg_demuxer_backend.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <variant>

TEST(FFmpegDependencyTest, CoreLibrariesAreUsable) {
    EXPECT_GT(avcodec_version(), 0U);
    EXPECT_GT(avformat_version(), 0U);
    EXPECT_GT(avutil_version(), 0U);
    EXPECT_NE(av_version_info(), nullptr);
}

TEST(FfmpegDemuxerBackendTest, ReportsOpenFailureWithoutKeepingResources) {
    semi::infra::ffmpeg::FfmpegDemuxerBackend backend;

    const auto failed = backend.open("this-file-does-not-exist.mp4");

    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().operation, semi::domain::DemuxerBackendOperation::Open);
    EXPECT_NE(failed.error().message, "");
    backend.close();
}

TEST(FfmpegDemuxerBackendTest, ProbesAudioStreamFromWavFile) {
    const auto path = std::filesystem::temp_directory_path() / "semi_player_demuxer_probe.wav";
    const std::array<unsigned char, 48> wav = {
        'R', 'I', 'F', 'F', 40, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
        0x40, 0x1F, 0, 0, 0x40, 0x1F, 0, 0, 1, 0, 8, 0,
        'd', 'a', 't', 'a', 4, 0, 0, 0, 128, 128, 128, 128,
    };
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
        ASSERT_TRUE(output.good());
    }

    semi::infra::ffmpeg::FfmpegDemuxerBackend backend;
    const auto probed = backend.open(path.string());

    ASSERT_TRUE(probed.has_value()) << probed.error().message;
    ASSERT_EQ(probed->streams.size(), 1U);
    const auto* audio = std::get_if<semi::domain::AudioCodecConfig>(&probed->streams.front().config);
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->sample_rate, 8000U);
    EXPECT_EQ(audio->channels, 1U);
    backend.close();
    EXPECT_TRUE(std::filesystem::remove(path));
}

TEST(FfmpegDemuxerBackendTest, ProbesCommittedMp4Fixture) {
    const std::filesystem::path path = SEMI_PLAYER_TEST_MEDIA_PATH;
    ASSERT_TRUE(std::filesystem::exists(path));

    semi::infra::ffmpeg::FfmpegDemuxerBackend backend;
    const auto probed = backend.open(path.string());

    ASSERT_TRUE(probed.has_value()) << probed.error().message;
    ASSERT_TRUE(probed->container.duration_us.has_value());
    EXPECT_GT(*probed->container.duration_us, 0);

    const auto video = std::find_if(probed->streams.begin(), probed->streams.end(), [](const auto& stream) {
        return std::holds_alternative<semi::domain::VideoCodecConfig>(stream.config);
    });
    ASSERT_NE(video, probed->streams.end());
    const auto& video_config = std::get<semi::domain::VideoCodecConfig>(video->config);
    EXPECT_EQ(video_config.coded_width, 320U);
    EXPECT_EQ(video_config.coded_height, 180U);

    const auto audio = std::find_if(probed->streams.begin(), probed->streams.end(), [](const auto& stream) {
        return std::holds_alternative<semi::domain::AudioCodecConfig>(stream.config);
    });
    ASSERT_NE(audio, probed->streams.end());
    const auto& audio_config = std::get<semi::domain::AudioCodecConfig>(audio->config);
    EXPECT_EQ(audio_config.sample_rate, 48000U);
    EXPECT_EQ(audio_config.channels, 1U);
    backend.close();
}
