extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <gtest/gtest.h>

TEST(FFmpegDependencyTest, CoreLibrariesAreUsable) {
    EXPECT_GT(avcodec_version(), 0U);
    EXPECT_GT(avformat_version(), 0U);
    EXPECT_GT(avutil_version(), 0U);
    EXPECT_NE(av_version_info(), nullptr);
}
