#include "infrastructure/ffmpeg/video_decoder/ffmpeg_d3d11_video_frame_buffer.hpp"
#include "infrastructure/ffmpeg/hardware_device/ffmpeg_d3d11_device_bridge.hpp"
#include "infrastructure/ffmpeg/video_renderer/ffmpeg_video_renderer_backend.hpp"
#include "infrastructure/gpu/d3d11/d3d11_device.hpp"
#include "domain/resource/video_frame_store/video_frame_store.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
}
#include <d3d11.h>
#include <gtest/gtest.h>
#include <cerrno>
#include <variant>

namespace semi::infra::ffmpeg::video_decoder {
namespace {

using contracts::media::resource_as;
using contracts::media::CpuVideoFrameResource;

TEST(FfmpegD3D11FrameErrorTest, RejectsMissingOwnersAndSoftwareFrames) {
    auto missing = FfmpegD3D11VideoFrameBuffer::create({}, {});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().native_code, AVERROR(EINVAL));
    AvFramePtr cpu(av_frame_alloc());
    ASSERT_NE(cpu, nullptr);
    cpu->format = AV_PIX_FMT_NV12;
    cpu->width = cpu->height = 64;
    ASSERT_EQ(av_frame_get_buffer(cpu.get(), 32), 0);
    auto software = FfmpegD3D11VideoFrameBuffer::create(std::move(cpu), {});
    EXPECT_FALSE(software.has_value());
}

class FfmpegD3D11FrameTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager = std::make_unique<gpu::GpuDeviceManager>();
        auto acquired = manager->acquire_d3d11();
        if (!acquired) GTEST_SKIP() << acquired.error().message;
        device = std::move(*acquired);
        auto bridged = hardware_device::make_d3d11_device_context(device);
        if (!bridged && bridged.error().native_code == AVERROR(ENOSYS)) {
            GTEST_SKIP() << bridged.error().message;
        }
        ASSERT_TRUE(bridged.has_value()) << bridged.error().message;
        reference = std::move(*bridged);
        pool.reset(av_hwframe_ctx_alloc(reference.get()));
        ASSERT_NE(pool, nullptr);
        auto* frames = reinterpret_cast<AVHWFramesContext*>(pool->data);
        frames->format = AV_PIX_FMT_D3D11;
        frames->sw_format = AV_PIX_FMT_NV12;
        frames->width = frames->height = 64;
        frames->initial_pool_size = 2;
        ASSERT_EQ(av_hwframe_ctx_init(pool.get()), 0);
    }

    AvFramePtr make_frame() {
        AvFramePtr frame(av_frame_alloc());
        if (!frame) return {};
        if (av_hwframe_get_buffer(pool.get(), frame.get(), 0) < 0) return {};
        return frame;
    }

    void release_sources() {
        pool.reset();
        reference.reset();
        device.reset();
        manager.reset();
    }

    std::unique_ptr<gpu::GpuDeviceManager> manager;
    std::shared_ptr<gpu::D3D11Device> device;
    AvBufferRefPtr reference;
    AvBufferRefPtr pool;
};

TEST_F(FfmpegD3D11FrameTest, DescribesNativeAndFfmpegResourcesWithoutCpuPlanes) {
    auto frame = make_frame();
    ASSERT_NE(frame, nullptr);
    const auto* original = frame.get();
    auto wrapped = FfmpegD3D11VideoFrameBuffer::create(std::move(frame), device);
    ASSERT_TRUE(wrapped.has_value()) << wrapped.error().message;
    const auto& buffer = **wrapped;
    EXPECT_EQ(buffer.width(), 64U);
    EXPECT_EQ(buffer.height(), 64U);
    EXPECT_EQ(buffer.pixel_format(), contracts::media::VideoPixelFormat::Nv12);
    EXPECT_EQ(resource_as<CpuVideoFrameResource>(buffer), nullptr);
    const auto* native = resource_as<gpu::D3D11VideoFrameResource>(buffer);
    const auto* ffmpeg = resource_as<FfmpegD3D11VideoFrameResource>(buffer);
    ASSERT_NE(native, nullptr);
    ASSERT_NE(ffmpeg, nullptr);
    EXPECT_EQ(static_cast<const gpu::D3D11VideoFrameResource*>(ffmpeg), native);
    EXPECT_EQ(ffmpeg->native_frame, original);
    EXPECT_EQ(native->device, device.get());
    EXPECT_EQ(native->texture, reinterpret_cast<ID3D11Texture2D*>(original->data[0]));
    EXPECT_EQ(native->array_slice, reinterpret_cast<std::uintptr_t>(original->data[1]));
    EXPECT_EQ(&buffer.resource(), &buffer.resource());
}

TEST_F(FfmpegD3D11FrameTest, StoreTransportsResourceAndKeepsOwnersAliveUntilPopIsReleased) {
    auto wrapped = FfmpegD3D11VideoFrameBuffer::create(make_frame(), device);
    ASSERT_TRUE(wrapped.has_value()) << wrapped.error().message;
    std::weak_ptr<gpu::D3D11Device> weak = device;
    const auto* description = &(*wrapped)->resource();
    domain::VideoFrameStore store({});
    domain::VideoFrameStoreItem item(domain::VideoFrame(
        contracts::media::DecodedVideo{.buffer = std::move(*wrapped), .pts_us = 321}, 7));
    ASSERT_EQ(store.try_push(std::move(item)), domain::VideoFramePushResult::Accepted);
    release_sources();
    ASSERT_FALSE(weak.expired());
    auto popped = store.try_pop();
    ASSERT_TRUE(popped.has_value());
    const auto& frame = std::get<domain::VideoFrame>(*popped);
    EXPECT_EQ(frame.generation(), 7U);
    EXPECT_EQ(frame.decoded().pts_us, 321);
    EXPECT_EQ(&frame.decoded().buffer->resource(), description);
    const auto* native = resource_as<gpu::D3D11VideoFrameResource>(*frame.decoded().buffer);
    ASSERT_NE(native, nullptr);
    EXPECT_EQ(native->device->native_device()->GetDeviceRemovedReason(), S_OK);
    popped.reset();
    EXPECT_TRUE(weak.expired());
}

TEST_F(FfmpegD3D11FrameTest, DestroyingStoreReleasesQueuedGpuFrames) {
    std::weak_ptr<gpu::D3D11Device> weak = device;
    auto wrapped = FfmpegD3D11VideoFrameBuffer::create(make_frame(), device);
    ASSERT_TRUE(wrapped.has_value());
    {
        domain::VideoFrameStore store({});
        domain::VideoFrameStoreItem item(domain::VideoFrame(
            contracts::media::DecodedVideo{.buffer = std::move(*wrapped), .pts_us = {}}, 1));
        ASSERT_EQ(store.try_push(std::move(item)), domain::VideoFramePushResult::Accepted);
        release_sources();
        EXPECT_FALSE(weak.expired());
    }
    EXPECT_TRUE(weak.expired());
}

TEST_F(FfmpegD3D11FrameTest, RetainingFramePreventsItsSurfaceFromBeingReused) {
    auto wrapped = FfmpegD3D11VideoFrameBuffer::create(make_frame(), device);
    ASSERT_TRUE(wrapped.has_value());
    const auto* first = resource_as<gpu::D3D11VideoFrameResource>(**wrapped);
    auto second = make_frame();
    ASSERT_NE(second, nullptr);
    EXPECT_TRUE(first->texture != reinterpret_cast<ID3D11Texture2D*>(second->data[0]) ||
                first->array_slice != reinterpret_cast<std::uintptr_t>(second->data[1]));
}

TEST_F(FfmpegD3D11FrameTest, DescribesP010UsingPoolPixelFormat) {
    UINT support = 0;
    if (FAILED(device->native_device()->CheckFormatSupport(DXGI_FORMAT_P010, &support)) ||
        !(support & D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
        GTEST_SKIP() << "Device does not support P010 textures";
    }
    AvBufferRefPtr p010_pool(av_hwframe_ctx_alloc(reference.get()));
    ASSERT_NE(p010_pool, nullptr);
    auto* frames = reinterpret_cast<AVHWFramesContext*>(p010_pool->data);
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = AV_PIX_FMT_P010LE;
    frames->width = frames->height = 64;
    frames->initial_pool_size = 2;
    ASSERT_EQ(av_hwframe_ctx_init(p010_pool.get()), 0);
    AvFramePtr frame(av_frame_alloc());
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(av_hwframe_get_buffer(p010_pool.get(), frame.get(), 0), 0);
    auto wrapped = FfmpegD3D11VideoFrameBuffer::create(std::move(frame), device);
    ASSERT_TRUE(wrapped.has_value()) << wrapped.error().message;
    EXPECT_EQ((*wrapped)->pixel_format(), contracts::media::VideoPixelFormat::P010);
    const auto* resource = resource_as<gpu::D3D11VideoFrameResource>(**wrapped);
    ASSERT_NE(resource, nullptr);
    D3D11_TEXTURE2D_DESC description{};
    resource->texture->GetDesc(&description);
    EXPECT_EQ(description.Format, DXGI_FORMAT_P010);
}

TEST_F(FfmpegD3D11FrameTest, RejectsFrameFromAnotherDevice) {
    gpu::GpuDeviceManager other_manager;
    auto other = other_manager.acquire_d3d11();
    ASSERT_TRUE(other.has_value());
    auto result = FfmpegD3D11VideoFrameBuffer::create(make_frame(), *other);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().native_code, AVERROR(EINVAL));
}

TEST_F(FfmpegD3D11FrameTest, RejectsInvalidSubresourceAndImageDimensions) {
    auto bad_slice = make_frame();
    ASSERT_NE(bad_slice, nullptr);
    bad_slice->data[1] = reinterpret_cast<std::uint8_t*>(std::intptr_t{999});
    EXPECT_FALSE(FfmpegD3D11VideoFrameBuffer::create(std::move(bad_slice), device).has_value());
    auto bad_size = make_frame();
    ASSERT_NE(bad_size, nullptr);
    bad_size->width = 65;
    EXPECT_FALSE(FfmpegD3D11VideoFrameBuffer::create(std::move(bad_size), device).has_value());
}

TEST_F(FfmpegD3D11FrameTest, RejectsUnsupportedPoolFormat) {
    auto frame = make_frame();
    ASSERT_NE(frame, nullptr);
    // Use separate uninitialized metadata; never mutate a live frame pool.
    AvBufferRefPtr invalid_pool(av_hwframe_ctx_alloc(reference.get()));
    ASSERT_NE(invalid_pool, nullptr);
    auto* frames = reinterpret_cast<AVHWFramesContext*>(invalid_pool->data);
    frames->format = AV_PIX_FMT_D3D11;
    frames->width = frames->height = 64;
    frames->sw_format = AV_PIX_FMT_YUV420P;
    av_buffer_unref(&frame->hw_frames_ctx);
    frame->hw_frames_ctx = invalid_pool.release();
    auto result = FfmpegD3D11VideoFrameBuffer::create(std::move(frame), device);
    EXPECT_FALSE(result.has_value());
}

TEST_F(FfmpegD3D11FrameTest, CpuScalerRejectsGpuResourceWithoutReadback) {
    auto wrapped = FfmpegD3D11VideoFrameBuffer::create(make_frame(), device);
    ASSERT_TRUE(wrapped.has_value());
    contracts::media::DecodedVideo input{.buffer = std::move(*wrapped), .pts_us = 123};
    video_renderer::FfmpegVideoRendererBackend scaler;
    ASSERT_TRUE(scaler.configure({}).has_value());
    auto result = scaler.render(input);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().operation, contracts::video_renderer::VideoRendererBackendOperation::Render);
    EXPECT_NE(result.error().message.find("CPU"), std::string::npos);
}

} // namespace
} // namespace semi::infra::ffmpeg::video_decoder
