#pragma once

#include "contracts/audio_decoder/audio_decoder_backend.hpp"

#include <memory>

namespace semi::infra::ffmpeg::audio_decoder {

class FfmpegAudioDecoderBackend final : public contracts::audio_decoder::AudioDecoderBackend {
public:
    FfmpegAudioDecoderBackend();
    ~FfmpegAudioDecoderBackend() override;

    [[nodiscard]] std::expected<void, contracts::audio_decoder::AudioDecoderBackendError>
    configure(const contracts::media::AudioCodecConfig& config) override;

    [[nodiscard]] std::expected<contracts::audio_decoder::DecodedAudioBatch,
                                 contracts::audio_decoder::AudioDecoderBackendError>
    decode(const contracts::demuxer::packet::EncodedPacket& packet) override;

    [[nodiscard]] std::expected<contracts::audio_decoder::DecodedAudioBatch,
                                 contracts::audio_decoder::AudioDecoderBackendError>
    drain() override;

    void reset() noexcept override;
    void unconfigure() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace semi::infra::ffmpeg::audio_decoder
