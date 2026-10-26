#include "infrastructure/gpu/readback/detail/owned_cpu_video_frame_buffer.hpp"

#include <gtest/gtest.h>
#include <stdexcept>

namespace semi::infra::gpu::detail {
namespace {

TEST(OwnedCpuVideoFrameBufferTest, DescribesOddSizedPlanesAndKeepsViewsStableAcrossOwnerTransfer) {
    using contracts::media::VideoPixelFormat;
    for (const auto format : {VideoPixelFormat::Nv12, VideoPixelFormat::P010}) {
        const auto scale = format == VideoPixelFormat::P010 ? 2U : 1U;
        auto frame = std::make_unique<OwnedCpuVideoFrameBuffer>(format, 5, 3);
        const auto* cpu = contracts::media::resource_as<contracts::media::CpuVideoFrameResource>(*frame);
        ASSERT_NE(cpu, nullptr);
        EXPECT_EQ(cpu->plane(0).stride_bytes, 5U * scale);
        EXPECT_EQ(cpu->plane(0).size_bytes, 15U * scale);
        EXPECT_EQ(cpu->plane(1).stride_bytes, 6U * scale);
        EXPECT_EQ(cpu->plane(1).size_bytes, 12U * scale);
        frame->writable_plane(1).back() = std::byte{123};
        std::unique_ptr<const contracts::media::VideoFrameBuffer> published = std::move(frame);
        EXPECT_EQ(cpu, contracts::media::resource_as<contracts::media::CpuVideoFrameResource>(*published));
        EXPECT_EQ(cpu->plane(1).bytes().back(), std::byte{123});
    }
}

TEST(OwnedCpuVideoFrameBufferTest, RejectsUnsupportedFormatsAndInvalidDimensions) {
    using contracts::media::VideoPixelFormat;
    EXPECT_THROW((OwnedCpuVideoFrameBuffer(VideoPixelFormat::Rgba8, 2, 2)), std::invalid_argument);
    EXPECT_THROW((OwnedCpuVideoFrameBuffer(VideoPixelFormat::Nv12, 0, 2)), std::invalid_argument);
    EXPECT_THROW((OwnedCpuVideoFrameBuffer(VideoPixelFormat::P010, UINT32_MAX, 2)), std::invalid_argument);
}

} // namespace
} // namespace semi::infra::gpu::detail
