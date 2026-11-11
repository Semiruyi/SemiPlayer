#pragma once

#include "contracts/video_readback/video_readback.hpp"

namespace semi::infra { class Notifier; }

namespace semi::infra::gpu {

// Native resource readback; no FFmpeg/domain dependency. Non-Windows builds
// report unsupported on use. Construction does not create a GPU device.
class D3D11VideoReadback final : public contracts::video_readback::VideoReadback {
public:
    explicit D3D11VideoReadback(std::shared_ptr<Notifier> notifier = {});
    ~D3D11VideoReadback() override;
    D3D11VideoReadback(const D3D11VideoReadback&) = delete;
    D3D11VideoReadback& operator=(const D3D11VideoReadback&) = delete;

    [[nodiscard]] std::expected<std::unique_ptr<const contracts::media::VideoFrameBuffer>,
                                contracts::video_readback::ReadbackError>
    readback(const contracts::media::VideoFrameBuffer& input) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace semi::infra::gpu
