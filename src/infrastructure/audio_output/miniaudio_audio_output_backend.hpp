#pragma once

#include "contracts/audio_output/audio_output_backend.hpp"

#include <memory>

namespace semi::infra::audio_output {

class MiniaudioAudioOutputBackend final : public contracts::audio_output::AudioOutputBackend {
public:
    explicit MiniaudioAudioOutputBackend(
        std::shared_ptr<contracts::audio_output::AudioOutputRealTimeNotifier> realtime_notifier);
    ~MiniaudioAudioOutputBackend() override;

    MiniaudioAudioOutputBackend(const MiniaudioAudioOutputBackend&) = delete;
    MiniaudioAudioOutputBackend& operator=(const MiniaudioAudioOutputBackend&) = delete;
    MiniaudioAudioOutputBackend(MiniaudioAudioOutputBackend&&) = delete;
    MiniaudioAudioOutputBackend& operator=(MiniaudioAudioOutputBackend&&) = delete;

    [[nodiscard]] std::expected<contracts::audio_output::AudioOutputConfigureResult,
                                contracts::audio_output::AudioOutputBackendError>
    configure(const contracts::audio_output::AudioOutputOptions& options) override;

    [[nodiscard]] std::expected<void, contracts::audio_output::AudioOutputBackendError>
    pause() override;

    [[nodiscard]] std::expected<void, contracts::audio_output::AudioOutputBackendError>
    resume() override;

    [[nodiscard]] std::expected<contracts::audio_output::AudioOutputSubmitStatus,
                                contracts::audio_output::AudioOutputBackendError>
    try_submit(const contracts::media::DecodedAudio& audio) override;

    [[nodiscard]] std::expected<contracts::audio_output::AudioOutputDrainStatus,
                                contracts::audio_output::AudioOutputBackendError>
    try_drain() override;

    [[nodiscard]] std::expected<void, contracts::audio_output::AudioOutputBackendError>
    reset() override;
    void unconfigure() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace semi::infra::audio_output
