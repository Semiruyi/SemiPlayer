#pragma once

#include "infrastructure/ffmpeg/ffmpeg_raii.hpp"
#include "infrastructure/gpu/d3d11/d3d11_video_frame_resource.hpp"

#include <expected>
#include <string>

namespace semi::infra::ffmpeg::video_decoder {

// Native FFmpeg access is only needed by FFmpeg consumers (e.g. readback).
// A D3D11 renderer can query the base resource without depending on FFmpeg.
struct FfmpegD3D11VideoFrameResource final : gpu::D3D11VideoFrameResource {
    FfmpegD3D11VideoFrameResource(gpu::D3D11Device* device, ID3D11Texture2D* texture,
                                std::uint32_t slice, const AVFrame* frame) noexcept
        : D3D11VideoFrameResource(device, texture, slice), native_frame(frame) {}

    const AVFrame* native_frame;
};

struct FfmpegD3D11FrameError {
    int native_code = 0; // FFmpeg error, not HRESULT.
    std::string message;
};

class FfmpegD3D11VideoFrameBuffer final : public contracts::media::VideoFrameBuffer {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<const FfmpegD3D11VideoFrameBuffer>,
                                       FfmpegD3D11FrameError>
    create(AvFramePtr frame, std::shared_ptr<gpu::D3D11Device> device);

    ~FfmpegD3D11VideoFrameBuffer() override;
    FfmpegD3D11VideoFrameBuffer(const FfmpegD3D11VideoFrameBuffer&) = delete;
    FfmpegD3D11VideoFrameBuffer& operator=(const FfmpegD3D11VideoFrameBuffer&) = delete;

    [[nodiscard]] contracts::media::VideoPixelFormat pixel_format() const noexcept override;
    [[nodiscard]] std::uint32_t width() const noexcept override;
    [[nodiscard]] std::uint32_t height() const noexcept override;
    [[nodiscard]] const contracts::media::VideoFrameResource& resource() const noexcept override;

private:
    FfmpegD3D11VideoFrameBuffer(AvFramePtr frame, std::shared_ptr<gpu::D3D11Device> device,
                               contracts::media::VideoPixelFormat format,
                               ID3D11Texture2D* texture, std::uint32_t slice) noexcept;

    // The frame (surface/pool references) is released before its device owner.
    std::shared_ptr<gpu::D3D11Device> device_;
    AvFramePtr frame_;
    contracts::media::VideoPixelFormat format_;
    FfmpegD3D11VideoFrameResource resource_;
};

} // namespace semi::infra::ffmpeg::video_decoder
