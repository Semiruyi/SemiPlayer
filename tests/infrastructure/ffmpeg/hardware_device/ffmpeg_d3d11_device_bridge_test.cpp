#include "infrastructure/ffmpeg/hardware_device/ffmpeg_d3d11_device_bridge.hpp"
#include "infrastructure/gpu/d3d11/d3d11_device.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <gtest/gtest.h>
#include <cerrno>
#include <future>

namespace semi::infra::ffmpeg::hardware_device {
namespace {

class FfmpegD3D11DeviceBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager = std::make_unique<gpu::GpuDeviceManager>();
        auto result = manager->acquire_d3d11();
        if (!result) GTEST_SKIP() << result.error().message << " HRESULT=" << result.error().native_code;
        device = std::move(*result);
        auto context = make_d3d11_device_context(device);
        if (!context && context.error().native_code == AVERROR(ENOSYS)) {
            GTEST_SKIP() << "FFmpeg was built without D3D11VA: " << context.error().message;
        }
        ASSERT_TRUE(context.has_value()) << context.error().message;
        reference = std::move(*context);
    }

    AVD3D11VADeviceContext* native() {
        return static_cast<AVD3D11VADeviceContext*>(
            reinterpret_cast<AVHWDeviceContext*>(reference->data)->hwctx);
    }

    std::unique_ptr<gpu::GpuDeviceManager> manager;
    std::shared_ptr<gpu::D3D11Device> device;
    AvBufferRefPtr reference;
};

TEST(FfmpegD3D11DeviceBridgeErrorTest, RejectsMissingDeviceWithoutCreatingOne) {
    auto result = make_d3d11_device_context({});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().native_code, AVERROR(EINVAL));
    EXPECT_FALSE(result.error().message.empty());
}

TEST_F(FfmpegD3D11DeviceBridgeTest, UsesOriginalDeviceAndImmediateContext) {
    EXPECT_EQ(native()->device, device->native_device());
    EXPECT_EQ(native()->device_context, device->native_context());
    EXPECT_NE(native()->video_device, nullptr);
    EXPECT_NE(native()->video_context, nullptr);
    EXPECT_EQ(native()->lock_ctx, &device->context_mutex());
}

TEST_F(FfmpegD3D11DeviceBridgeTest, CallbacksShareRecursiveLockWithNativeConsumers) {
    auto guard = device->lock_context();
    native()->lock(native()->lock_ctx);
    native()->unlock(native()->lock_ctx);
    guard.unlock();

    native()->lock(native()->lock_ctx);
    auto competing = std::async(std::launch::async, [this] {
        std::unique_lock other(device->context_mutex(), std::try_to_lock);
        return other.owns_lock();
    });
    const bool acquired = competing.get();
    native()->unlock(native()->lock_ctx);
    EXPECT_FALSE(acquired);
    EXPECT_TRUE(device->lock_context().owns_lock());
}

TEST_F(FfmpegD3D11DeviceBridgeTest, FramePoolKeepsDeviceAndCallbacksAlive) {
    std::weak_ptr<gpu::D3D11Device> weak = device;
    AvBufferRefPtr pool(av_hwframe_ctx_alloc(reference.get()));
    ASSERT_NE(pool, nullptr);
    auto* frames = reinterpret_cast<AVHWFramesContext*>(pool->data);
    auto* callbacks = static_cast<AVD3D11VADeviceContext*>(frames->device_ctx->hwctx);
    reference.reset();
    device.reset();
    manager.reset();
    ASSERT_FALSE(weak.expired());
    callbacks->lock(callbacks->lock_ctx);
    callbacks->unlock(callbacks->lock_ctx);
    EXPECT_EQ(callbacks->device->GetDeviceRemovedReason(), S_OK);
    pool.reset();
    EXPECT_TRUE(weak.expired());
}

TEST_F(FfmpegD3D11DeviceBridgeTest, IndependentBridgesShareDeviceAndSurvivePeerRelease) {
    auto second = make_d3d11_device_context(device);
    ASSERT_TRUE(second.has_value());
    auto* other = static_cast<AVD3D11VADeviceContext*>(
        reinterpret_cast<AVHWDeviceContext*>((*second)->data)->hwctx);
    EXPECT_EQ(other->device, native()->device);
    EXPECT_EQ(other->lock_ctx, native()->lock_ctx);
    reference.reset();
    EXPECT_EQ(other->device->GetDeviceRemovedReason(), S_OK);
    other->lock(other->lock_ctx);
    other->unlock(other->lock_ctx);
}

TEST_F(FfmpegD3D11DeviceBridgeTest, HardwareFrameRetainsDeviceAndCanBeReadBack) {
    std::weak_ptr<gpu::D3D11Device> weak = device;
    AvBufferRefPtr pool(av_hwframe_ctx_alloc(reference.get()));
    ASSERT_NE(pool, nullptr);
    auto* frames = reinterpret_cast<AVHWFramesContext*>(pool->data);
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = 64;
    frames->height = 64;
    frames->initial_pool_size = 2;
    ASSERT_EQ(av_hwframe_ctx_init(pool.get()), 0);
    AvFramePtr frame(av_frame_alloc());
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(av_hwframe_get_buffer(pool.get(), frame.get(), 0), 0);
    ASSERT_NE(frame->data[0], nullptr);

    pool.reset();
    reference.reset();
    device.reset();
    manager.reset();
    ASSERT_FALSE(weak.expired());
    AvFramePtr cpu(av_frame_alloc());
    ASSERT_NE(cpu, nullptr);
    ASSERT_EQ(av_hwframe_transfer_data(cpu.get(), frame.get(), 0), 0);
    EXPECT_EQ(cpu->format, AV_PIX_FMT_NV12);
    EXPECT_EQ(cpu->width, 64);
    EXPECT_EQ(cpu->height, 64);
    EXPECT_NE(cpu->data[0], nullptr);
    frame.reset();
    EXPECT_TRUE(weak.expired());
}

} // namespace
} // namespace semi::infra::ffmpeg::hardware_device
