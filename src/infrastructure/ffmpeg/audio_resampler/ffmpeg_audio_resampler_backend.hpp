#pragma once

#include "contracts/audio_resampler/audio_resampler_backend.hpp"

#include <memory>

namespace semi::infra::ffmpeg::audio_resampler {

class FfmpegAudioResamplerBackend final
    : public contracts::audio_resampler::AudioResamplerBackend {
public:
    FfmpegAudioResamplerBackend();
    ~FfmpegAudioResamplerBackend() override;

    [[nodiscard]] std::expected<void, contracts::audio_resampler::AudioResamplerBackendError>
    configure(const contracts::media::AudioPcmFormat& input_format,
              const contracts::media::AudioPcmFormat& output_format) override;

    [[nodiscard]] std::expected<contracts::audio_resampler::ResampledAudioBatch,
                                 contracts::audio_resampler::AudioResamplerBackendError>
    resample(const contracts::media::DecodedAudio& input) override;

    [[nodiscard]] std::expected<contracts::audio_resampler::ResampledAudioBatch,
                                 contracts::audio_resampler::AudioResamplerBackendError>
    drain() override;

    void reset() noexcept override;
    void unconfigure() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace semi::infra::ffmpeg::audio_resampler
