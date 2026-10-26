#include "infrastructure/ffmpeg/video_renderer/ffmpeg_video_renderer_backend.hpp"
#include "contracts/video_readback/video_readback.hpp"

#include <gtest/gtest.h>
#include <array>

namespace semi::infra::ffmpeg::video_renderer {
namespace {

using contracts::media::VideoFrameBuffer;
using contracts::media::VideoPixelFormat;
using contracts::media::VideoFrameResource;
using contracts::media::DecodedVideo;
using contracts::video_readback::VideoReadback;
using contracts::video_readback::ReadbackError;
using contracts::video_readback::ReadbackOperation;

class TestNonCpuFrame final : public VideoFrameBuffer {
public:
    VideoPixelFormat pixel_format() const noexcept override { return VideoPixelFormat::Rgba8; }
    std::uint32_t width() const noexcept override { return 2; }
    std::uint32_t height() const noexcept override { return 1; }
    const VideoFrameResource& resource() const noexcept override { return resource_; }
private:
    VideoFrameResource resource_;
};

class TestCpuFrame final : public VideoFrameBuffer {
public:
    explicit TestCpuFrame(std::shared_ptr<int> lifetime = {}) : lifetime_(std::move(lifetime)) {}
    VideoPixelFormat pixel_format() const noexcept override { return VideoPixelFormat::Rgba8; }
    std::uint32_t width() const noexcept override { return image_width; }
    std::uint32_t height() const noexcept override { return 1; }
    const VideoFrameResource& resource() const noexcept override { return resource_; }
    std::uint32_t image_width = 2;
    void truncate() { view_.size_bytes = 1; }
private:
    std::shared_ptr<int> lifetime_;
    std::array<std::byte, 72> bytes_{std::byte{55}, std::byte{66}, std::byte{77}, std::byte{255},
                                     std::byte{88}, std::byte{99}, std::byte{111}, std::byte{255}};
    contracts::media::VideoPlaneView view_{bytes_.data(), 8, 8};
    contracts::media::CpuVideoFrameResource resource_{std::span(&view_, 1)};
};

class FakeReadback final : public VideoReadback {
public:
    enum class Mode { Success, Failure, Null, NonCpu, ChangedMetadata, Truncated };
    Mode mode = Mode::Success;
    int calls = 0;
    const VideoFrameBuffer* last_input = nullptr;
    std::weak_ptr<int> output_lifetime;

    std::expected<std::unique_ptr<const VideoFrameBuffer>, ReadbackError>
    readback(const VideoFrameBuffer& input) override {
        ++calls;
        last_input = &input;
        if (mode == Mode::Failure) {
            return std::unexpected(ReadbackError{ReadbackOperation::Map, -123, "native Map failed"});
        }
        if (mode == Mode::Null) return std::unique_ptr<const VideoFrameBuffer>{};
        if (mode == Mode::NonCpu) return std::unique_ptr<const VideoFrameBuffer>(new TestNonCpuFrame);
        auto token = std::make_shared<int>(1);
        output_lifetime = token;
        auto result = std::make_unique<TestCpuFrame>(std::move(token));
        if (mode == Mode::ChangedMetadata) result->image_width = 1;
        if (mode == Mode::Truncated) result->truncate();
        return std::unique_ptr<const VideoFrameBuffer>(std::move(result));
    }
};

TEST(FfmpegRendererReadbackTest, CpuInputBypassesInjectedReadback) {
    auto reader = std::make_shared<FakeReadback>();
    reader->mode = FakeReadback::Mode::Failure;
    FfmpegVideoRendererBackend renderer(reader);
    ASSERT_TRUE(renderer.configure({}).has_value());
    auto result = renderer.render(DecodedVideo{.buffer = std::make_unique<TestCpuFrame>(), .pts_us = 72});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(reader->calls, 0);
    EXPECT_EQ(result->pixels[0], std::byte{55});
    EXPECT_EQ(result->pts_us, 72);
}

TEST(FfmpegRendererReadbackTest, ConvertsReturnedCpuImageAndPreservesInputTimestamp) {
    auto reader = std::make_shared<FakeReadback>();
    FfmpegVideoRendererBackend renderer(reader);
    ASSERT_TRUE(renderer.configure({}).has_value());
    DecodedVideo input{.buffer = std::make_unique<TestNonCpuFrame>(), .pts_us = 987};
    const auto* original = input.buffer.get();
    auto result = renderer.render(input);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(reader->calls, 1);
    EXPECT_EQ(reader->last_input, original);
    EXPECT_EQ(input.buffer.get(), original);
    EXPECT_EQ(result->pts_us, 987);
    EXPECT_EQ(result->pixels[0], std::byte{55});
    EXPECT_EQ(result->pixels[4], std::byte{88});
    EXPECT_TRUE(reader->output_lifetime.expired());
    renderer.reset();
    EXPECT_TRUE(renderer.render(input).has_value());
    EXPECT_EQ(reader->calls, 2);
    renderer.unconfigure();
    EXPECT_FALSE(renderer.render(input).has_value());
    EXPECT_EQ(reader->calls, 2);
}

TEST(FfmpegRendererReadbackTest, PropagatesNativeReadbackFailureAsRenderError) {
    auto reader = std::make_shared<FakeReadback>();
    reader->mode = FakeReadback::Mode::Failure;
    FfmpegVideoRendererBackend renderer(reader);
    ASSERT_TRUE(renderer.configure({}).has_value());
    auto result = renderer.render(DecodedVideo{.buffer = std::make_unique<TestNonCpuFrame>(), .pts_us = {}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().operation, contracts::video_renderer::VideoRendererBackendOperation::Render);
    EXPECT_EQ(result.error().native_code, -123);
    EXPECT_NE(result.error().message.find("native Map failed"), std::string::npos);
}

TEST(FfmpegRendererReadbackTest, RejectsInvalidResultsFromReadbackImplementation) {
    for (const auto mode : {FakeReadback::Mode::Null, FakeReadback::Mode::NonCpu,
                           FakeReadback::Mode::ChangedMetadata, FakeReadback::Mode::Truncated}) {
        SCOPED_TRACE(static_cast<int>(mode));
        auto reader = std::make_shared<FakeReadback>();
        reader->mode = mode;
        FfmpegVideoRendererBackend renderer(reader);
        ASSERT_TRUE(renderer.configure({}).has_value());
        auto result = renderer.render(DecodedVideo{.buffer = std::make_unique<TestNonCpuFrame>(), .pts_us = {}});
        EXPECT_FALSE(result.has_value());
    }
}

} // namespace
} // namespace semi::infra::ffmpeg::video_renderer
