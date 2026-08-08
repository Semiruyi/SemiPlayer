#include "infrastructure/audio_output/miniaudio_audio_output_backend.hpp"
#include "infrastructure/audio_output/byte_spsc_ring.hpp"

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio/miniaudio.h>

#include <cstddef>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>

namespace semi::infra::audio_output {
namespace {

using contracts::audio_output::AudioOutputBackendError;
using contracts::audio_output::AudioOutputBackendOperation;
using contracts::audio_output::AudioOutputConfigureResult;
using contracts::audio_output::AudioOutputDrainStatus;
using contracts::audio_output::AudioOutputOptions;
using contracts::audio_output::AudioOutputSubmitStatus;
using contracts::media::AudioPcmFormat;
using contracts::media::AudioSampleFormat;
using contracts::media::DecodedAudio;

constexpr std::uint32_t kPlaybackSampleRate = 48'000;
constexpr std::uint32_t kPlaybackChannels = 2;
constexpr std::size_t kBytesPerSample = sizeof(float);
constexpr std::size_t kRingBufferSeconds = 2;

AudioPcmFormat default_playback_format() noexcept {
    return AudioPcmFormat{
        .sample_rate = kPlaybackSampleRate,
        .channels = kPlaybackChannels,
        .sample_format = AudioSampleFormat::F32,
        .planar = false,
    };
}

std::size_t frame_size_bytes(const AudioPcmFormat& format) noexcept {
    switch (format.sample_format) {
    case AudioSampleFormat::U8:
        return format.channels;
    case AudioSampleFormat::S16:
        return format.channels * sizeof(std::int16_t);
    case AudioSampleFormat::S32:
        return format.channels * sizeof(std::int32_t);
    case AudioSampleFormat::S64:
        return format.channels * sizeof(std::int64_t);
    case AudioSampleFormat::F32:
        return format.channels * sizeof(float);
    case AudioSampleFormat::F64:
        return format.channels * sizeof(double);
    case AudioSampleFormat::Unknown:
        return 0;
    }
    return 0;
}

bool same_format(const AudioPcmFormat& lhs, const AudioPcmFormat& rhs) noexcept {
    return lhs.sample_rate == rhs.sample_rate && lhs.channels == rhs.channels &&
           lhs.sample_format == rhs.sample_format && lhs.planar == rhs.planar;
}

AudioOutputBackendError make_error(AudioOutputBackendOperation operation,
                                   int native_code,
                                   std::string message) {
    return AudioOutputBackendError{
        .operation = operation,
        .native_code = native_code,
        .message = std::move(message),
    };
}

} // namespace

struct MiniaudioAudioOutputBackend::Impl {
    explicit Impl(std::shared_ptr<contracts::audio_output::AudioOutputRealTimeNotifier> notifier)
        : realtime_notifier(std::move(notifier)) {}

    ~Impl() { unconfigure(); }

    std::expected<AudioOutputConfigureResult, AudioOutputBackendError>
    configure(const AudioOutputOptions& options) {
        std::lock_guard lock(mutex);
        if (configured) {
            return std::unexpected(make_error(
                AudioOutputBackendOperation::Configure,
                0,
                "miniaudio output backend is already configured"));
        }
        if (options.device_id.has_value()) {
            return std::unexpected(make_error(
                AudioOutputBackendOperation::Configure,
                0,
                "miniaudio output backend device_id selection is not implemented yet"));
        }

        playback_format = default_playback_format();
        buffer.resize(playback_format.sample_rate * playback_format.channels * kBytesPerSample *
                      kRingBufferSeconds);

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = playback_format.channels;
        config.sampleRate = playback_format.sample_rate;
        config.dataCallback = &Impl::data_callback;
        config.pUserData = this;

        ma_result result = ma_device_init(nullptr, &config, &device);
        if (result != MA_SUCCESS) {
            buffer.clear();
            return std::unexpected(make_error(
                AudioOutputBackendOperation::Configure,
                static_cast<int>(result),
                "miniaudio device initialization failed"));
        }
        device_initialized = true;
        configured = true;
        return AudioOutputConfigureResult{.playback_format = playback_format};
    }

    std::expected<void, AudioOutputBackendError> pause() {
        std::lock_guard lock(mutex);
        if (!configured) {
            return std::unexpected(make_error(
                AudioOutputBackendOperation::Pause,
                0,
                "miniaudio output backend is not configured"));
        }
        if (!device_running) {
            return {};
        }

        const ma_result result = ma_device_stop(&device);
        if (result != MA_SUCCESS) {
            return std::unexpected(make_error(
                AudioOutputBackendOperation::Pause,
                static_cast<int>(result),
                "miniaudio device pause failed"));
        }
        device_running = false;
        return {};
    }

    std::expected<void, AudioOutputBackendError> resume() {
        std::lock_guard lock(mutex);
        if (!configured) {
            return std::unexpected(make_error(
                AudioOutputBackendOperation::Resume,
                0,
                "miniaudio output backend is not configured"));
        }
        if (device_running) {
            return {};
        }

        const ma_result result = ma_device_start(&device);
        if (result != MA_SUCCESS) {
            return std::unexpected(make_error(
                AudioOutputBackendOperation::Resume,
                static_cast<int>(result),
                "miniaudio device resume failed"));
        }
        device_running = true;
        return {};
    }

    std::expected<AudioOutputSubmitStatus, AudioOutputBackendError>
    try_submit(const DecodedAudio& audio) {
        std::lock_guard lock(mutex);
        if (!configured) {
            return std::unexpected(make_error(
                AudioOutputBackendOperation::Submit,
                0,
                "miniaudio output backend is not configured"));
        }
        if (!same_format(audio.format, playback_format)) {
            return std::unexpected(make_error(
                AudioOutputBackendOperation::Submit,
                0,
                "miniaudio output backend received an unexpected PCM format"));
        }
        if (audio.format.planar || audio.planes.size() != 1) {
            return std::unexpected(make_error(
                AudioOutputBackendOperation::Submit,
                0,
                "miniaudio output backend requires one packed PCM plane"));
        }

        const std::size_t expected_bytes =
            static_cast<std::size_t>(audio.samples_per_channel) * frame_size_bytes(audio.format);
        if (audio.planes.front().size() != expected_bytes) {
            return std::unexpected(make_error(
                AudioOutputBackendOperation::Submit,
                0,
                "miniaudio output backend received malformed PCM plane data"));
        }
        if (expected_bytes == 0) {
            return AudioOutputSubmitStatus::Accepted;
        }
        if (!buffer.try_write(audio.planes.front().data(), expected_bytes)) {
            return AudioOutputSubmitStatus::WouldBlock;
        }
        return AudioOutputSubmitStatus::Accepted;
    }

    std::expected<AudioOutputDrainStatus, AudioOutputBackendError> try_drain() {
        std::lock_guard lock(mutex);
        if (!configured) {
            return std::unexpected(make_error(
                AudioOutputBackendOperation::Drain,
                0,
                "miniaudio output backend is not configured"));
        }
        return buffer.available() == 0 ? AudioOutputDrainStatus::Drained
                                   : AudioOutputDrainStatus::WouldBlock;
    }

    std::expected<void, AudioOutputBackendError> reset() {
        const bool was_running = device_running;
        if (device_initialized && was_running) {
            // ma_device_stop() waits for the device callback thread, so no
            // pre-reset callback can notify after the old buffer is cleared.
            const ma_result stopped = ma_device_stop(&device);
            if (stopped != MA_SUCCESS) {
                return std::unexpected(make_error(
                    AudioOutputBackendOperation::Reset,
                    static_cast<int>(stopped),
                    "miniaudio device reset stop failed"));
            }
            device_running = false;
        }
        buffer.reset();
        previous_callback_copied_frames.store(0, std::memory_order_release);
        if (device_initialized && was_running) {
            const ma_result started = ma_device_start(&device);
            if (started != MA_SUCCESS) {
                device_running = false;
                return std::unexpected(make_error(
                    AudioOutputBackendOperation::Reset,
                    static_cast<int>(started),
                    "miniaudio device reset restart failed"));
            }
            device_running = true;
        }
        return {};
    }

    void unconfigure() noexcept {
        if (device_initialized && device_running) {
            (void)ma_device_stop(&device);
            device_running = false;
        }
        if (device_initialized) {
            ma_device_uninit(&device);
            device_initialized = false;
        }
        std::lock_guard lock(mutex);
        configured = false;
        playback_format = {};
        buffer.clear();
        previous_callback_copied_frames.store(0, std::memory_order_release);
    }

    static void data_callback(ma_device* device,
                              void* output,
                              const void*,
                              ma_uint32 frame_count) noexcept {
        auto* self = static_cast<Impl*>(device->pUserData);
        if (self == nullptr || output == nullptr) {
            return;
        }
        self->write_to_device(output, frame_count);
    }

    void write_to_device(void* output, ma_uint32 frame_count) noexcept {
        const auto confirmed_frames = previous_callback_copied_frames.load(
            std::memory_order_acquire);
        const std::size_t requested_bytes =
            static_cast<std::size_t>(frame_count) * kPlaybackChannels * kBytesPerSample;
        auto* bytes = static_cast<std::byte*>(output);

        const std::size_t copied = buffer.try_read(bytes, requested_bytes);
        if (copied < requested_bytes) {
            std::memset(bytes + copied, 0, requested_bytes - copied);
        }
        const std::size_t bytes_per_frame = kPlaybackChannels * kBytesPerSample;
        previous_callback_copied_frames.store(
            static_cast<std::uint32_t>(copied / bytes_per_frame),
            std::memory_order_release);
        if (confirmed_frames > 0 && realtime_notifier) {
            realtime_notifier->notify(confirmed_frames);
        }
    }

    std::mutex mutex;
    std::shared_ptr<contracts::audio_output::AudioOutputRealTimeNotifier> realtime_notifier;
    AudioPcmFormat playback_format{};
    ByteSpscRing buffer;
    std::atomic<std::uint32_t> previous_callback_copied_frames{0};
    ma_device device{};
    bool device_initialized = false;
    bool device_running = false;
    bool configured = false;
};

MiniaudioAudioOutputBackend::MiniaudioAudioOutputBackend(
    std::shared_ptr<contracts::audio_output::AudioOutputRealTimeNotifier> realtime_notifier)
    : impl_(std::make_unique<Impl>(std::move(realtime_notifier))) {}

MiniaudioAudioOutputBackend::~MiniaudioAudioOutputBackend() = default;

std::expected<contracts::audio_output::AudioOutputConfigureResult,
              contracts::audio_output::AudioOutputBackendError>
MiniaudioAudioOutputBackend::configure(const contracts::audio_output::AudioOutputOptions& options) {
    return impl_->configure(options);
}

std::expected<void, contracts::audio_output::AudioOutputBackendError>
MiniaudioAudioOutputBackend::pause() {
    return impl_->pause();
}

std::expected<void, contracts::audio_output::AudioOutputBackendError>
MiniaudioAudioOutputBackend::resume() {
    return impl_->resume();
}

std::expected<contracts::audio_output::AudioOutputSubmitStatus,
              contracts::audio_output::AudioOutputBackendError>
MiniaudioAudioOutputBackend::try_submit(
    const contracts::media::DecodedAudio& audio) {
    return impl_->try_submit(audio);
}

std::expected<contracts::audio_output::AudioOutputDrainStatus,
              contracts::audio_output::AudioOutputBackendError>
MiniaudioAudioOutputBackend::try_drain() {
    return impl_->try_drain();
}

std::expected<void, contracts::audio_output::AudioOutputBackendError>
MiniaudioAudioOutputBackend::reset() {
    return impl_->reset();
}

void MiniaudioAudioOutputBackend::unconfigure() noexcept {
    impl_->unconfigure();
}

} // namespace semi::infra::audio_output
