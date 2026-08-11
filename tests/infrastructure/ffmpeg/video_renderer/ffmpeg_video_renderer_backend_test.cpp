#include "infrastructure/ffmpeg/ffmpeg_raii.hpp"
#include "infrastructure/ffmpeg/video_decoder/ffmpeg_video_frame_buffer.hpp"
#include "infrastructure/ffmpeg/video_renderer/ffmpeg_video_renderer_backend.hpp"

#include <gtest/gtest.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace semi::infra::ffmpeg::video_renderer {
namespace {

using contracts::media::DecodedVideo;
using contracts::media::VideoPixelFormat;
using contracts::video_renderer::VideoRendererBackendOperation;

class RgbaFrameBuffer final : public contracts::media::VideoFrameBuffer {
public:
    RgbaFrameBuffer(std::uint32_t width,
                    std::uint32_t height,
                    std::uint32_t stride,
                    std::vector<std::byte> bytes)
        : width_(width), height_(height), stride_(stride), bytes_(std::move(bytes)) {}

    [[nodiscard]] VideoPixelFormat pixel_format() const noexcept override {
        return VideoPixelFormat::Rgba8;
    }

    [[nodiscard]] std::uint32_t width() const noexcept override { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept override { return height_; }
    [[nodiscard]] std::size_t plane_count() const noexcept override { return 1; }

    [[nodiscard]] contracts::media::VideoPlaneView
    plane(std::size_t index) const noexcept override {
        if (index != 0) {
            return {};
        }
        return contracts::media::VideoPlaneView{
            .data = bytes_.data(),
            .size_bytes = bytes_.size(),
            .stride_bytes = stride_,
        };
    }

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t stride_ = 0;
    std::vector<std::byte> bytes_;
};

DecodedVideo make_rgba_video(std::vector<std::byte> bytes,
                             std::uint32_t width = 2,
                             std::uint32_t height = 1,
                             std::uint32_t stride = 8,
                             std::optional<std::int64_t> pts = 123) {
    return DecodedVideo{
        .buffer = std::make_unique<RgbaFrameBuffer>(width, height, stride, std::move(bytes)),
        .pts_us = pts,
    };
}

TEST(FfmpegVideoRendererBackendTest, RejectsRenderBeforeConfiguration) {
    FfmpegVideoRendererBackend backend;

    const auto rendered = backend.render(make_rgba_video({
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{255},
        std::byte{4}, std::byte{5}, std::byte{6}, std::byte{255}}));

    ASSERT_FALSE(rendered.has_value());
    EXPECT_EQ(rendered.error().operation, VideoRendererBackendOperation::Render);
}

TEST(FfmpegVideoRendererBackendTest, ConvertsRgbaAndPreservesTimelineMetadata) {
    FfmpegVideoRendererBackend backend;
    ASSERT_TRUE(backend.configure({}).has_value());

    const auto rendered = backend.render(make_rgba_video({
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{255},
        std::byte{4}, std::byte{5}, std::byte{6}, std::byte{255}}));
    ASSERT_TRUE(rendered.has_value()) << rendered.error().message;

    EXPECT_EQ(rendered->pixel_format, VideoPixelFormat::Rgba8);
    EXPECT_EQ(rendered->width, 2U);
    EXPECT_EQ(rendered->height, 1U);
    EXPECT_EQ(rendered->stride_bytes, 8U);
    ASSERT_EQ(rendered->pixels.size(), 8U);
    EXPECT_EQ(rendered->pixels[0], std::byte{1});
    EXPECT_EQ(rendered->pixels[1], std::byte{2});
    EXPECT_EQ(rendered->pixels[2], std::byte{3});
    EXPECT_EQ(rendered->pixels[4], std::byte{4});
    EXPECT_EQ(rendered->pts_us, 123);
}

TEST(FfmpegVideoRendererBackendTest, ConvertsYuv420pToRgba) {
    AvFramePtr frame(av_frame_alloc());
    ASSERT_NE(frame, nullptr);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 2;
    frame->height = 2;
    ASSERT_EQ(av_frame_get_buffer(frame.get(), 1), 0);
    ASSERT_EQ(av_frame_make_writable(frame.get()), 0);

    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            frame->data[0][row * frame->linesize[0] + column] = 128;
        }
    }
    frame->data[1][0] = 128;
    frame->data[2][0] = 128;

    DecodedVideo input{
        .buffer = std::make_unique<video_decoder::FfmpegVideoFrameBuffer>(std::move(frame)),
        .pts_us = 777,
    };

    FfmpegVideoRendererBackend backend;
    ASSERT_TRUE(backend.configure({}).has_value());
    const auto rendered = backend.render(input);
    ASSERT_TRUE(rendered.has_value()) << rendered.error().message;

    EXPECT_EQ(rendered->width, 2U);
    EXPECT_EQ(rendered->height, 2U);
    EXPECT_EQ(rendered->stride_bytes, 8U);
    ASSERT_EQ(rendered->pixels.size(), 16U);
    EXPECT_EQ(rendered->pixels[3], std::byte{255});
    EXPECT_EQ(rendered->pixels[7], std::byte{255});
    EXPECT_EQ(rendered->pixels[11], std::byte{255});
    EXPECT_EQ(rendered->pixels[15], std::byte{255});
    EXPECT_EQ(rendered->pts_us, 777);
}

TEST(FfmpegVideoRendererBackendTest, RejectsTruncatedInputPlane) {
    FfmpegVideoRendererBackend backend;
    ASSERT_TRUE(backend.configure({}).has_value());

    const auto rendered = backend.render(make_rgba_video(
        {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{255}}, 2, 1, 8));

    ASSERT_FALSE(rendered.has_value());
    EXPECT_EQ(rendered.error().operation, VideoRendererBackendOperation::Render);
}

TEST(FfmpegVideoRendererBackendTest, ResetKeepsBackendReusable) {
    FfmpegVideoRendererBackend backend;
    ASSERT_TRUE(backend.configure({}).has_value());

    const auto first = backend.render(make_rgba_video({
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{255},
        std::byte{4}, std::byte{5}, std::byte{6}, std::byte{255}}));
    ASSERT_TRUE(first.has_value());

    backend.reset();
    const auto second = backend.render(make_rgba_video({
        std::byte{7}, std::byte{8}, std::byte{9}, std::byte{255},
        std::byte{10}, std::byte{11}, std::byte{12}, std::byte{255}}));
    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_EQ(second->pixels[0], std::byte{7});
}

} // namespace
} // namespace semi::infra::ffmpeg::video_renderer
