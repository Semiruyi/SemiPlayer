#include "infrastructure/gpu/gpu_device_manager.hpp"

#if defined(_WIN32)
#include "infrastructure/gpu/d3d11/d3d11_device.hpp"
#endif
#include "infrastructure/log/log.hpp"

#define SEMI_LOG_TAG "gpu_device"

namespace semi::infra::gpu {

std::expected<std::shared_ptr<D3D11Device>, GpuDeviceError>
GpuDeviceManager::acquire_d3d11() {
#if defined(_WIN32)
    std::lock_guard lock(mutex_);
    if (d3d11_) return d3d11_;

    auto device = D3D11Device::create();
    if (!device) {
        SEMI_LOG_WARN("D3D11 device creation failed: HRESULT={} {}",
                      device.error().native_code, device.error().message);
        return std::unexpected(std::move(device.error()));
    }
    d3d11_ = std::move(*device);
    SEMI_LOG_INFO("D3D11 hardware device created");
    return d3d11_;
#else
    // The manager exists on every platform; only its native backends vary.
    return std::unexpected(GpuDeviceError{0, "D3D11 is not supported on this platform"});
#endif
}

} // namespace semi::infra::gpu
