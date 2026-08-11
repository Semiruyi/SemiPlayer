#include "contracts/video_renderer/video_renderer_backend.hpp"

#include <gtest/gtest.h>

namespace semi::contracts::video_renderer {
namespace {

class FakeVideoRendererBackend final : public VideoRendererBackend {
public:
    std::expected<void, VideoRendererBackendError>
    configure(const VideoRendererOptions&) override {
        return {};
    }

    std::expected<media::RenderedVideo, VideoRendererBackendError>
    render(const media::DecodedVideo&) override {
        return media::RenderedVideo{
            .pixel_format = media::VideoPixelFormat::Rgba8,
            .width = 1,
            .height = 1,
            .stride_bytes = 4,
            .pixels = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{255}},
            .pts_us = 10,
        };
    }

    void reset() noexcept override {}
    void unconfigure() noexcept override {}
};

TEST(VideoRendererBackendContract, RepresentsRenderedHostFrame) {
    FakeVideoRendererBackend backend;

    ASSERT_TRUE(backend.configure({}).has_value());
    const auto rendered = backend.render({});
    ASSERT_TRUE(rendered.has_value());
    EXPECT_EQ(rendered->pixel_format, media::VideoPixelFormat::Rgba8);
    EXPECT_EQ(rendered->width, 1U);
    EXPECT_EQ(rendered->height, 1U);
    EXPECT_EQ(rendered->stride_bytes, 4U);
    EXPECT_EQ(rendered->pixels.size(), 4U);
    EXPECT_EQ(rendered->pts_us, 10);
}

TEST(VideoRendererBackendContract, PreservesOperationInStructuredError) {
    const VideoRendererBackendError error{
        .operation = VideoRendererBackendOperation::Configure,
        .native_code = -123,
        .message = "renderer configure failed",
    };

    EXPECT_EQ(error.operation, VideoRendererBackendOperation::Configure);
    EXPECT_EQ(error.native_code, -123);
    EXPECT_EQ(error.message, "renderer configure failed");
}

} // namespace
} // namespace semi::contracts::video_renderer
