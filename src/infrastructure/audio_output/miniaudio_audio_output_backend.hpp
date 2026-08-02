#pragma once

#include "contracts/audio_output/audio_output_backend.hpp"

#include <memory>

namespace semi::infra::audio_output {

class MiniaudioAudioOutputBackend final : public contracts::audio_output::AudioOutputBackend {
public:
    MiniaudioAudioOutputBackend();
    ~MiniaudioAudioOutputBackend() override;

    MiniaudioAudioOutputBackend(const MiniaudioAudioOutputBackend&) = delete;
    MiniaudioAudioOutputBackend& operator=(const MiniaudioAudioOutputBackend&) = delete;
    MiniaudioAudioOutputBackend(MiniaudioAudioOutputBackend&&) = delete;
    MiniaudioAudioOutputBackend& operator=(MiniaudioAudioOutputBackend&&) = delete;

    void set_progress_notifier(
        contracts::audio_output::AudioOutputBackendProgressNotifier* notifier) noexcept override;

    [[nodiscard]] std::expected<contracts::audio_output::AudioOutputConfigureResult,
                                contracts::audio_output::AudioOutputBackendError>
    configure(const contracts::audio_output::AudioOutputOptions& options) override;

    [[nodiscard]] std::expected<contracts::audio_output::AudioOutputSubmitStatus,
                                contracts::audio_output::AudioOutputBackendError>
    try_submit(const contracts::media::DecodedAudio& audio) override;

    [[nodiscard]] std::expected<contracts::audio_output::AudioOutputDrainStatus,
                                contracts::audio_output::AudioOutputBackendError>
    try_drain() override;

    void reset() noexcept override;
    void unconfigure() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace semi::infra::audio_output
