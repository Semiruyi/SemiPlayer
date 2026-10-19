#pragma once

#include "infrastructure/gpu/gpu_device_manager.hpp"

// Native interfaces stay inside infrastructure; consumers need no Windows
// headers just to retain a device owner.
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace semi::infra::gpu {

class D3D11Device final {
public:
    using ContextGuard = std::unique_lock<std::recursive_mutex>;

    ~D3D11Device();
    D3D11Device(const D3D11Device&) = delete;
    D3D11Device& operator=(const D3D11Device&) = delete;

    [[nodiscard]] ID3D11Device* native_device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* native_context() const noexcept;
    [[nodiscard]] ContextGuard lock_context();

    // For native adapter lock/unlock callbacks only. Keep this device alive
    // for as long as any callback can use the mutex.
    [[nodiscard]] std::recursive_mutex& context_mutex() noexcept;

private:
    friend class GpuDeviceManager;
    struct Impl;
    explicit D3D11Device(std::unique_ptr<Impl> impl) noexcept;
    [[nodiscard]] static std::expected<std::shared_ptr<D3D11Device>, GpuDeviceError> create();

    std::unique_ptr<Impl> impl_;
};

} // namespace semi::infra::gpu
