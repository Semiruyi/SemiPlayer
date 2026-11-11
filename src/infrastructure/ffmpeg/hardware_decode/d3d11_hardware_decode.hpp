#pragma once

#include "infrastructure/ffmpeg/hardware_decode/ffmpeg_hardware_decode.hpp"

namespace semi::infra::ffmpeg::hardware_decode {

class D3D11HardwareDecodeProvider final : public FfmpegHardwareDecodeProvider {
public:
    explicit D3D11HardwareDecodeProvider(std::shared_ptr<gpu::GpuDeviceManager> devices);
    [[nodiscard]] std::expected<std::unique_ptr<FfmpegHardwareDecodeSession>, HardwareDecodeError>
    prepare(const AVCodec& codec) override;

private:
    std::shared_ptr<gpu::GpuDeviceManager> devices_;
};

} // namespace semi::infra::ffmpeg::hardware_decode
