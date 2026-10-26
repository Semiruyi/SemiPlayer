#include "infrastructure/gpu/readback/d3d11_video_readback.hpp"

#if defined(_WIN32)
#include "infrastructure/gpu/d3d11/d3d11_device.hpp"
#include "infrastructure/gpu/d3d11/d3d11_video_frame_resource.hpp"
#include "infrastructure/gpu/readback/detail/owned_cpu_video_frame_buffer.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#endif

namespace semi::infra::gpu {
namespace {
using contracts::video_readback::ReadbackError;
using contracts::video_readback::ReadbackOperation;

std::unexpected<ReadbackError> error(ReadbackOperation operation, std::int32_t code,
                                     const char* message) {
    return std::unexpected(ReadbackError{operation, code, message});
}

#if defined(_WIN32)
struct UnmapOnExit {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    ~UnmapOnExit() { context->Unmap(texture, 0); }
};
#endif
} // namespace

struct D3D11VideoReadback::Impl {
#if defined(_WIN32)
    // COM owners keep the native identity stable. Never cache borrowed
    // D3D11Device wrapper pointers or frame/resource descriptions.
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC layout{};
#endif
};

D3D11VideoReadback::D3D11VideoReadback() : impl_(std::make_unique<Impl>()) {}
D3D11VideoReadback::~D3D11VideoReadback() = default;

std::expected<std::unique_ptr<const contracts::media::VideoFrameBuffer>, ReadbackError>
D3D11VideoReadback::readback(const contracts::media::VideoFrameBuffer& input) {
#if defined(_WIN32)
    using contracts::media::VideoPixelFormat;
    const auto* source = contracts::media::resource_as<D3D11VideoFrameResource>(input);
    if (!source || !source->device || !source->texture) {
        return error(ReadbackOperation::Validate, 0, "D3D11 readback requires a valid D3D11 resource");
    }
    DXGI_FORMAT format;
    if (input.pixel_format() == VideoPixelFormat::Nv12) format = DXGI_FORMAT_NV12;
    else if (input.pixel_format() == VideoPixelFormat::P010) format = DXGI_FORMAT_P010;
    else return error(ReadbackOperation::Validate, 0, "D3D11 readback supports NV12 and P010 only");

    D3D11_TEXTURE2D_DESC layout{};
    source->texture->GetDesc(&layout);
    Microsoft::WRL::ComPtr<ID3D11Device> texture_device;
    source->texture->GetDevice(texture_device.GetAddressOf());
    if (texture_device.Get() != source->device->native_device() || layout.Format != format ||
        source->array_slice >= layout.ArraySize || layout.MipLevels != 1 ||
        layout.SampleDesc.Count != 1 || (layout.Width % 2) != 0 || (layout.Height % 2) != 0 ||
        input.width() == 0 || input.height() == 0 ||
        input.width() > layout.Width || input.height() > layout.Height) {
        return error(ReadbackOperation::Validate, 0, "D3D11 frame device, format, slice or dimensions are invalid");
    }

    try {
        auto output = std::make_unique<detail::OwnedCpuVideoFrameBuffer>(
            input.pixel_format(), input.width(), input.height());
        if (!impl_->staging || impl_->device.Get() != texture_device.Get() ||
            impl_->layout.Width != layout.Width || impl_->layout.Height != layout.Height ||
            impl_->layout.Format != layout.Format) {
            D3D11_TEXTURE2D_DESC desc = layout;
            desc.ArraySize = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
            const HRESULT status = texture_device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf());
            if (FAILED(status)) {
                return error(ReadbackOperation::CreateStaging, static_cast<std::int32_t>(status),
                             "D3D11 CreateTexture2D for readback staging failed");
            }
            impl_->staging = std::move(staging);
            impl_->device = std::move(texture_device);
            impl_->layout = layout;
        }

        auto lock = source->device->lock_context();
        auto* context = source->device->native_context();
        context->CopySubresourceRegion(impl_->staging.Get(), 0, 0, 0, 0,
                                      source->texture, source->array_slice, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT status = context->Map(impl_->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(status)) {
            return error(ReadbackOperation::Map, static_cast<std::int32_t>(status),
                         "D3D11 Map for readback failed");
        }
        UnmapOnExit unmap{context, impl_->staging.Get()};
        const auto* cpu = contracts::media::resource_as<contracts::media::CpuVideoFrameResource>(*output);
        const auto maximum = std::numeric_limits<std::size_t>::max();
        const auto total_rows = static_cast<std::size_t>(layout.Height) + layout.Height / 2;
        if (!mapped.pData || mapped.RowPitch < cpu->plane(1).stride_bytes ||
            total_rows > maximum / mapped.RowPitch) {
            return error(ReadbackOperation::Map, 0, "D3D11 mapped layout has invalid pointer or row pitch");
        }
        const auto* y = static_cast<const std::byte*>(mapped.pData);
        const auto* uv = y + static_cast<std::size_t>(mapped.RowPitch) * layout.Height;
        for (std::size_t plane = 0; plane < 2; ++plane) {
            const auto view = cpu->plane(plane);
            auto destination = output->writable_plane(plane);
            const auto rows = plane == 0 ? input.height() : (static_cast<std::size_t>(input.height()) + 1) / 2;
            const auto* data = plane == 0 ? y : uv;
            for (std::size_t row = 0; row < rows; ++row) {
                std::memcpy(destination.data() + row * view.stride_bytes,
                            data + row * mapped.RowPitch, view.stride_bytes);
            }
        }
        return std::unique_ptr<const contracts::media::VideoFrameBuffer>(std::move(output));
    } catch (const std::bad_alloc&) {
        return error(ReadbackOperation::Allocate, static_cast<std::int32_t>(E_OUTOFMEMORY),
                     "allocating CPU readback image failed");
    } catch (const std::length_error&) {
        return error(ReadbackOperation::Allocate, static_cast<std::int32_t>(E_OUTOFMEMORY),
                     "CPU readback image exceeds allocation limit");
    } catch (const std::invalid_argument& exception) {
        return error(ReadbackOperation::Validate, 0, exception.what());
    }
#else
    (void)input;
    return error(ReadbackOperation::Validate, 0, "D3D11 readback is not supported on this platform");
#endif
}

} // namespace semi::infra::gpu
