#pragma once

#include "contracts/audio_output/audio_output_backend.hpp"

#include <memory>

namespace semi::infra::audio_output {

class NullAudioOutputBackend final : public contracts::audio_output::AudioOutputBackend {
public:
    explicit NullAudioOutputBackend(
        std::shared_ptr<contracts::audio_output::AudioOutputRealTimeNotifier> realtime_notifier);
    ~NullAudioOutputBackend() override;

    NullAudioOutputBackend(const NullAudioOutputBackend&) = delete;
    NullAudioOutputBackend& operator=(const NullAudioOutputBackend&) = delete;
    NullAudioOutputBackend(NullAudioOutputBackend&&) = delete;
    NullAudioOutputBackend& operator=(NullAudioOutputBackend&&) = delete;

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
    [[nodiscard]] contracts::audio_output::AudioOutputBackendError
    state_error(contracts::audio_output::AudioOutputBackendOperation operation,
                const char* message) const;

    std::shared_ptr<contracts::audio_output::AudioOutputRealTimeNotifier> realtime_notifier_;
    contracts::media::AudioPcmFormat playback_format_{};
    bool configured_ = false;
};

} // namespace semi::infra::audio_output
