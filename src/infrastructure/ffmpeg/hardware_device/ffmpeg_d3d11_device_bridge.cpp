#include "infrastructure/ffmpeg/hardware_device/ffmpeg_d3d11_device_bridge.hpp"

#include "infrastructure/gpu/d3d11/d3d11_device.hpp"
#include "infrastructure/log/log.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <array>
#include <cerrno>
#include <new>
#include <utility>

#define SEMI_LOG_TAG "ffmpeg_device"

namespace semi::infra::ffmpeg::hardware_device {
namespace {

struct BridgeState {
    std::shared_ptr<gpu::D3D11Device> device;
};

void release_bridge(AVHWDeviceContext* context) {
    delete static_cast<BridgeState*>(context->user_opaque);
}

void lock_context(void* mutex) { static_cast<std::recursive_mutex*>(mutex)->lock(); }
void unlock_context(void* mutex) { static_cast<std::recursive_mutex*>(mutex)->unlock(); }

std::unexpected<FfmpegDeviceBridgeError> bridge_error(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
    av_strerror(code, message.data(), message.size());
    SEMI_LOG_WARN("D3D11VA device bridge failed: FFmpeg={} {}", code, message.data());
    return std::unexpected(FfmpegDeviceBridgeError{code, message.data()});
}

} // namespace

std::expected<AvBufferRefPtr, FfmpegDeviceBridgeError>
make_d3d11_device_context(std::shared_ptr<gpu::D3D11Device> device) {
    if (!device) return bridge_error(AVERROR(EINVAL));

    bool supported = false;
    for (auto type = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE);
         type != AV_HWDEVICE_TYPE_NONE; type = av_hwdevice_iterate_types(type)) {
        if (type == AV_HWDEVICE_TYPE_D3D11VA) { supported = true; break; }
    }
    if (!supported) return bridge_error(AVERROR(ENOSYS));

    try {
        auto state = std::make_unique<BridgeState>(std::move(device));
        AvBufferRefPtr reference(av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA));
        if (!reference) return bridge_error(AVERROR(ENOMEM));
        auto* context = reinterpret_cast<AVHWDeviceContext*>(reference->data);
        auto* native = static_cast<AVD3D11VADeviceContext*>(context->hwctx);

        // FFmpeg releases its own interface reference, not our borrowed one.
        native->device = state->device->native_device();
        native->device->AddRef();
        native->lock = lock_context;
        native->unlock = unlock_context;
        native->lock_ctx = &state->device->context_mutex();
        context->free = release_bridge;
        context->user_opaque = state.release();

        // FFmpeg derives context/video interfaces from the supplied device.
        const int status = av_hwdevice_ctx_init(reference.get());
        if (status < 0) return bridge_error(status);
        return reference;
    } catch (const std::bad_alloc&) {
        return bridge_error(AVERROR(ENOMEM));
    }
}

} // namespace semi::infra::ffmpeg::hardware_device
