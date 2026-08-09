#pragma once

#include "contracts/video_decoder/video_decoder_backend.hpp"

#include <memory>

namespace semi::infra::ffmpeg::video_decoder {

class FfmpegVideoDecoderBackend final : public contracts::video_decoder::VideoDecoderBackend {
public:
    FfmpegVideoDecoderBackend();
    ~FfmpegVideoDecoderBackend() override;

    [[nodiscard]] std::expected<void, contracts::video_decoder::VideoDecoderBackendError>
    configure(const contracts::media::VideoCodecConfig& config) override;

    [[nodiscard]] std::expected<contracts::video_decoder::DecodedVideoBatch,
                                contracts::video_decoder::VideoDecoderBackendError>
    decode(const contracts::demuxer::packet::EncodedPacket& packet) override;

    [[nodiscard]] std::expected<contracts::video_decoder::DecodedVideoBatch,
                                contracts::video_decoder::VideoDecoderBackendError>
    drain() override;

    void reset() noexcept override;
    void unconfigure() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace semi::infra::ffmpeg::video_decoder
