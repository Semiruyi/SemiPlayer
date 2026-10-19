#include "infrastructure/gpu/d3d11/d3d11_device.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <new>
#include <utility>

namespace semi::infra::gpu {

struct D3D11Device::Impl {
    std::recursive_mutex context_mutex;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
};

D3D11Device::D3D11Device(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
D3D11Device::~D3D11Device() = default;

std::expected<std::shared_ptr<D3D11Device>, GpuDeviceError> D3D11Device::create() {
    try {
        auto impl = std::make_unique<Impl>();
        // Hardware only. Never use SINGLETHREADED: multiple backends will
        // share this device and serialize immediate-context calls explicitly.
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        const HRESULT status = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            impl->device.GetAddressOf(), nullptr, impl->context.GetAddressOf());
        if (FAILED(status)) {
            return std::unexpected(GpuDeviceError{
                static_cast<std::int32_t>(status), "D3D11CreateDevice failed (default hardware adapter)"});
        }
        return std::shared_ptr<D3D11Device>(new D3D11Device(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(GpuDeviceError{
            static_cast<std::int32_t>(E_OUTOFMEMORY), "allocating D3D11 device owner failed"});
    }
}

ID3D11Device* D3D11Device::native_device() const noexcept { return impl_->device.Get(); }
ID3D11DeviceContext* D3D11Device::native_context() const noexcept { return impl_->context.Get(); }
D3D11Device::ContextGuard D3D11Device::lock_context() { return ContextGuard(impl_->context_mutex); }
std::recursive_mutex& D3D11Device::context_mutex() noexcept { return impl_->context_mutex; }

} // namespace semi::infra::gpu
