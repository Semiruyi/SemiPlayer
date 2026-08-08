#include "infrastructure/audio_output/null_audio_output_backend.hpp"

namespace semi::infra::audio_output {
namespace {

contracts::media::AudioPcmFormat default_playback_format() noexcept {
    return contracts::media::AudioPcmFormat{
        .sample_rate = 48000,
        .channels = 2,
        .sample_format = contracts::media::AudioSampleFormat::F32,
        .planar = false,
    };
}

bool same_format(const contracts::media::AudioPcmFormat& lhs,
                 const contracts::media::AudioPcmFormat& rhs) noexcept {
    return lhs.sample_rate == rhs.sample_rate && lhs.channels == rhs.channels &&
           lhs.sample_format == rhs.sample_format && lhs.planar == rhs.planar;
}

} // namespace

NullAudioOutputBackend::NullAudioOutputBackend(
    std::shared_ptr<contracts::audio_output::AudioOutputRealTimeNotifier> realtime_notifier)
    : realtime_notifier_(std::move(realtime_notifier)) {}

NullAudioOutputBackend::~NullAudioOutputBackend() {
    unconfigure();
}

std::expected<contracts::audio_output::AudioOutputConfigureResult,
              contracts::audio_output::AudioOutputBackendError>
NullAudioOutputBackend::configure(const contracts::audio_output::AudioOutputOptions&) {
    if (configured_) {
        return std::unexpected(state_error(
            contracts::audio_output::AudioOutputBackendOperation::Configure,
            "null audio output backend is already configured"));
    }

    playback_format_ = default_playback_format();
    configured_ = true;
    return contracts::audio_output::AudioOutputConfigureResult{
        .playback_format = playback_format_,
    };
}

std::expected<void, contracts::audio_output::AudioOutputBackendError>
NullAudioOutputBackend::pause() {
    if (!configured_) {
        return std::unexpected(state_error(
            contracts::audio_output::AudioOutputBackendOperation::Pause,
            "null audio output backend is not configured"));
    }
    return {};
}

std::expected<void, contracts::audio_output::AudioOutputBackendError>
NullAudioOutputBackend::resume() {
    if (!configured_) {
        return std::unexpected(state_error(
            contracts::audio_output::AudioOutputBackendOperation::Resume,
            "null audio output backend is not configured"));
    }
    return {};
}

std::expected<contracts::audio_output::AudioOutputSubmitStatus,
              contracts::audio_output::AudioOutputBackendError>
NullAudioOutputBackend::try_submit(
    const contracts::audio_output::AudioOutputSubmission& submission) {
    const auto& audio = submission.audio;
    if (!configured_) {
        return std::unexpected(state_error(
            contracts::audio_output::AudioOutputBackendOperation::Submit,
            "null audio output backend is not configured"));
    }
    if (!same_format(audio.format, playback_format_)) {
        return std::unexpected(state_error(
            contracts::audio_output::AudioOutputBackendOperation::Submit,
            "null audio output backend received an unexpected PCM format"));
    }

    if (realtime_notifier_) {
        realtime_notifier_->notify(contracts::audio_output::AudioFramesConsumed{
            .generation = submission.generation,
            .first_pts_us = audio.pts_us,
            .frames = audio.samples_per_channel,
            .sample_rate = audio.format.sample_rate,
        });
    }
    return contracts::audio_output::AudioOutputSubmitStatus::Accepted;
}

std::expected<contracts::audio_output::AudioOutputDrainStatus,
              contracts::audio_output::AudioOutputBackendError>
NullAudioOutputBackend::try_drain() {
    if (!configured_) {
        return std::unexpected(state_error(
            contracts::audio_output::AudioOutputBackendOperation::Drain,
            "null audio output backend is not configured"));
    }

    return contracts::audio_output::AudioOutputDrainStatus::Drained;
}

std::expected<void, contracts::audio_output::AudioOutputBackendError>
NullAudioOutputBackend::reset() {
    return {};
}

void NullAudioOutputBackend::unconfigure() noexcept {
    playback_format_ = {};
    configured_ = false;
}

contracts::audio_output::AudioOutputBackendError
NullAudioOutputBackend::state_error(
    contracts::audio_output::AudioOutputBackendOperation operation,
    const char* message) const {
    return contracts::audio_output::AudioOutputBackendError{
        .operation = operation,
        .native_code = 0,
        .message = message,
    };
}

} // namespace semi::infra::audio_output
