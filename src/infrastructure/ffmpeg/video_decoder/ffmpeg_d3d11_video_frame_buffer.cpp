#include "infrastructure/ffmpeg/video_decoder/ffmpeg_d3d11_video_frame_buffer.hpp"

#include "infrastructure/gpu/d3d11/d3d11_device.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <wrl/client.h>
#include <cerrno>
#include <new>
#include <utility>

namespace semi::infra::ffmpeg::video_decoder {
namespace {

std::unexpected<FfmpegD3D11FrameError> invalid_frame(const char* message) {
    return std::unexpected(FfmpegD3D11FrameError{AVERROR(EINVAL), message});
}
} // namespace

std::expected<std::unique_ptr<const FfmpegD3D11VideoFrameBuffer>, FfmpegD3D11FrameError>
FfmpegD3D11VideoFrameBuffer::create(AvFramePtr frame, std::shared_ptr<gpu::D3D11Device> device) {
    if (!frame || !device || frame->format != AV_PIX_FMT_D3D11 ||
        frame->width <= 0 || frame->height <= 0 || !frame->buf[0] ||
        !frame->data[0] || !frame->hw_frames_ctx) {
        return invalid_frame("D3D11 frame requires an owned hardware surface, frame pool and device");
    }
    const auto* pool = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
    if (!pool || pool->format != AV_PIX_FMT_D3D11 || !pool->device_ctx ||
        pool->device_ctx->type != AV_HWDEVICE_TYPE_D3D11VA || !pool->device_ctx->hwctx ||
        frame->width > pool->width || frame->height > pool->height) {
        return invalid_frame("D3D11 frame pool metadata is invalid");
    }
    const auto* native = static_cast<const AVD3D11VADeviceContext*>(pool->device_ctx->hwctx);
    if (native->device != device->native_device() || native->lock_ctx != &device->context_mutex()) {
        return invalid_frame(
            "D3D11 frame pool does not belong to the supplied device and synchronization state");
    }

    contracts::media::VideoPixelFormat format;
    DXGI_FORMAT native_format;
    switch (pool->sw_format) {
    case AV_PIX_FMT_NV12:
        format = contracts::media::VideoPixelFormat::Nv12;
        native_format = DXGI_FORMAT_NV12;
        break;
    case AV_PIX_FMT_P010LE:
        format = contracts::media::VideoPixelFormat::P010;
        native_format = DXGI_FORMAT_P010;
        break;
    default:
        return invalid_frame("D3D11 frame pool pixel format is unsupported (expected NV12 or P010)");
    }

    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const auto slice = reinterpret_cast<std::intptr_t>(frame->data[1]);
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    Microsoft::WRL::ComPtr<ID3D11Device> texture_device;
    texture->GetDevice(texture_device.GetAddressOf());
    if (texture_device.Get() != device->native_device() || slice < 0 ||
        static_cast<std::uintptr_t>(slice) >= description.ArraySize ||
        description.MipLevels != 1 || description.SampleDesc.Count != 1 ||
        description.Format != native_format ||
        static_cast<std::uint32_t>(pool->width) > description.Width ||
        static_cast<std::uint32_t>(pool->height) > description.Height) {
        return invalid_frame("D3D11 texture device, subresource or layout does not match the frame pool");
    }

    try {
        return std::unique_ptr<const FfmpegD3D11VideoFrameBuffer>(new FfmpegD3D11VideoFrameBuffer(
            std::move(frame), std::move(device), format, texture, static_cast<std::uint32_t>(slice)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            FfmpegD3D11FrameError{AVERROR(ENOMEM), "allocating D3D11 frame owner failed"});
    }
}

FfmpegD3D11VideoFrameBuffer::FfmpegD3D11VideoFrameBuffer(
    AvFramePtr frame, std::shared_ptr<gpu::D3D11Device> device,
    contracts::media::VideoPixelFormat format, ID3D11Texture2D* texture, std::uint32_t slice) noexcept
    : device_(std::move(device)), frame_(std::move(frame)), format_(format),
      resource_(device_.get(), texture, slice, frame_.get()) {}

FfmpegD3D11VideoFrameBuffer::~FfmpegD3D11VideoFrameBuffer() = default;

contracts::media::VideoPixelFormat FfmpegD3D11VideoFrameBuffer::pixel_format() const noexcept {
    return format_;
}

std::uint32_t FfmpegD3D11VideoFrameBuffer::width() const noexcept {
    return static_cast<std::uint32_t>(frame_->width);
}

std::uint32_t FfmpegD3D11VideoFrameBuffer::height() const noexcept {
    return static_cast<std::uint32_t>(frame_->height);
}

const contracts::media::VideoFrameResource& FfmpegD3D11VideoFrameBuffer::resource() const noexcept {
    return resource_;
}

} // namespace semi::infra::ffmpeg::video_decoder
