#include "infrastructure/ffmpeg/hardware_decode/d3d11_hardware_decode.hpp"

#include "infrastructure/ffmpeg/hardware_device/ffmpeg_d3d11_device_bridge.hpp"
#include "infrastructure/ffmpeg/video_decoder/ffmpeg_d3d11_video_frame_buffer.hpp"
#include "infrastructure/gpu/gpu_device_manager.hpp"
#include "infrastructure/log/log.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <cerrno>
#include <new>
#include <utility>

#define SEMI_LOG_TAG "ffmpeg_d3d11_decode"

namespace semi::infra::ffmpeg::hardware_decode {
namespace {

class D3D11HardwareDecodeSession final : public FfmpegHardwareDecodeSession {
public:
    D3D11HardwareDecodeSession(std::shared_ptr<gpu::D3D11Device> device, AvBufferRefPtr bridge)
        : device_(std::move(device)), bridge_(std::move(bridge)) {}

    std::string_view name() const noexcept override { return "d3d11va"; }

    std::expected<void, HardwareDecodeError> configure(AVCodecContext& context) override {
        context.hw_device_ctx = av_buffer_ref(bridge_.get());
        if (!context.hw_device_ctx) {
            return std::unexpected(HardwareDecodeError{AVERROR(ENOMEM), "retaining hardware device failed"});
        }
        // FrameStore holds 4, renderer holds 1, with 3 additional slots for
        // handoff/batch output. Codec reference and reorder surfaces are extra.
        context.extra_hw_frames = 8;
        return {};
    }

    AVPixelFormat select_format(AVCodecContext& context,
                                const AVPixelFormat* candidates) noexcept override {
        if (!candidates) return AV_PIX_FMT_NONE;
        for (auto* candidate = candidates; *candidate != AV_PIX_FMT_NONE; ++candidate) {
            if (*candidate != AV_PIX_FMT_D3D11) continue;
            AVBufferRef* raw_pool = nullptr;
            int status = avcodec_get_hw_frames_parameters(
                &context, bridge_.get(), *candidate, &raw_pool);
            AvBufferRefPtr pool(raw_pool);
            if (status < 0) {
                SEMI_LOG_INFO("D3D11 frame parameters unavailable: FFmpeg={}", status);
                return AV_PIX_FMT_NONE;
            }
            auto* parameters = reinterpret_cast<AVHWFramesContext*>(pool->data);
            // Consumers currently support only these native layouts. Inspect
            // the actual pool format, not only the coded software pixel format.
            if (parameters->sw_format != AV_PIX_FMT_NV12 &&
                parameters->sw_format != AV_PIX_FMT_P010LE) {
                SEMI_LOG_INFO("D3D11 output layout unsupported: format={}",
                              static_cast<int>(parameters->sw_format));
                return AV_PIX_FMT_NONE;
            }
            // avcodec_get_hw_frames_parameters already includes extra_hw_frames
            // and frame-thread buffering. Do not add the output budget twice.
            status = av_hwframe_ctx_init(pool.get());
            if (status < 0) {
                SEMI_LOG_INFO("D3D11 frame pool unavailable: FFmpeg={}", status);
                return AV_PIX_FMT_NONE;
            }
            // FFmpeg clears hw_frames_ctx before every get_format call.
            context.hw_frames_ctx = pool.release();
            return *candidate;
        }
        return AV_PIX_FMT_NONE;
    }

    std::expected<std::unique_ptr<const contracts::media::VideoFrameBuffer>, HardwareDecodeError>
    wrap_frame(AvFramePtr frame) override {
        auto wrapped = video_decoder::FfmpegD3D11VideoFrameBuffer::create(std::move(frame), device_);
        if (!wrapped) {
            return std::unexpected(HardwareDecodeError{
                wrapped.error().native_code, std::move(wrapped.error().message)});
        }
        return std::unique_ptr<const contracts::media::VideoFrameBuffer>(std::move(*wrapped));
    }

private:
    std::shared_ptr<gpu::D3D11Device> device_;
    AvBufferRefPtr bridge_;
};

} // namespace

D3D11HardwareDecodeProvider::D3D11HardwareDecodeProvider(
    std::shared_ptr<gpu::GpuDeviceManager> devices) : devices_(std::move(devices)) {}

std::expected<std::unique_ptr<FfmpegHardwareDecodeSession>, HardwareDecodeError>
D3D11HardwareDecodeProvider::prepare(const AVCodec& codec) {
    bool supported = false;
    for (int index = 0; const auto* config = avcodec_get_hw_config(&codec, index); ++index) {
        if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
            config->pix_fmt == AV_PIX_FMT_D3D11 &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            supported = true;
            break;
        }
    }
    if (!supported || !devices_) {
        return std::unexpected(HardwareDecodeError{AVERROR(ENOSYS), "D3D11VA unavailable for decoder"});
    }
    try {
        auto device = devices_->acquire_d3d11();
        if (!device) {
            return std::unexpected(HardwareDecodeError{
                AVERROR_EXTERNAL, device.error().message + " (native=" +
                std::to_string(device.error().native_code) + ")"});
        }
        auto bridge = hardware_device::make_d3d11_device_context(*device);
        if (!bridge) {
            return std::unexpected(HardwareDecodeError{
                bridge.error().native_code, std::move(bridge.error().message)});
        }
        return std::unique_ptr<FfmpegHardwareDecodeSession>(
            std::make_unique<D3D11HardwareDecodeSession>(std::move(*device), std::move(*bridge)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(HardwareDecodeError{AVERROR(ENOMEM), "preparing D3D11VA failed"});
    }
}

} // namespace semi::infra::ffmpeg::hardware_decode
