#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>

namespace semi::infra::gpu {

class D3D11Device;

struct GpuDeviceError {
    // Native HRESULT for D3D11; zero when no native call was made (unsupported
    // platform). Never an FFmpeg error code. expected determines success.
    std::int32_t native_code = 0;
    std::string message;
};

// Infrastructure-only, lazy device cache. No playback or codec policy.
class GpuDeviceManager final {
public:
    GpuDeviceManager() = default;
    GpuDeviceManager(const GpuDeviceManager&) = delete;
    GpuDeviceManager& operator=(const GpuDeviceManager&) = delete;

    [[nodiscard]] std::expected<std::shared_ptr<D3D11Device>, GpuDeviceError>
    acquire_d3d11();

private:
    std::mutex mutex_;
    std::shared_ptr<D3D11Device> d3d11_;
};

} // namespace semi::infra::gpu
