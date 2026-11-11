#include "infrastructure/ffmpeg/hardware_decode/ffmpeg_hardware_decode.hpp"

#if defined(_WIN32)
#include "infrastructure/ffmpeg/hardware_decode/d3d11_hardware_decode.hpp"
#endif

namespace semi::infra::ffmpeg::hardware_decode {

std::shared_ptr<FfmpegHardwareDecodeProvider>
make_platform_hardware_decode_provider(std::shared_ptr<gpu::GpuDeviceManager> devices) {
#if defined(_WIN32)
    return std::make_shared<D3D11HardwareDecodeProvider>(std::move(devices));
#else
    (void)devices;
    return {};
#endif
}

} // namespace semi::infra::ffmpeg::hardware_decode
