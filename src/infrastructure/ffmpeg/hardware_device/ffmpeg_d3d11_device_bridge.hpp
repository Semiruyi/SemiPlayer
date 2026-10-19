#pragma once

#include "infrastructure/ffmpeg/ffmpeg_raii.hpp"

#include <expected>
#include <memory>
#include <string>

namespace semi::infra::gpu { class D3D11Device; }

namespace semi::infra::ffmpeg::hardware_device {

struct FfmpegDeviceBridgeError {
    int native_code = 0; // FFmpeg error, not HRESULT.
    std::string message;
};

// The returned FFmpeg context retains the device owner (and its mutex) until
// its last reference is released, including references retained by frame pools.
[[nodiscard]] std::expected<AvBufferRefPtr, FfmpegDeviceBridgeError>
make_d3d11_device_context(std::shared_ptr<gpu::D3D11Device> device);

} // namespace semi::infra::ffmpeg::hardware_device
