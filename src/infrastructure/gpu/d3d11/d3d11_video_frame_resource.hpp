#pragma once

#include "contracts/media/media_types.hpp"

struct ID3D11Texture2D;

namespace semi::infra::gpu {

class D3D11Device;

// Borrowed resource description, valid while the enclosing frame owner lives.
// Acquiring this view neither maps memory nor waits for GPU completion.
struct D3D11VideoFrameResource : contracts::media::VideoFrameResource {
    D3D11VideoFrameResource(D3D11Device* owner, ID3D11Texture2D* image,
                           std::uint32_t slice) noexcept
        : device(owner), texture(image), array_slice(slice) {}

    D3D11Device* device;
    ID3D11Texture2D* texture;
    std::uint32_t array_slice;
};

} // namespace semi::infra::gpu
