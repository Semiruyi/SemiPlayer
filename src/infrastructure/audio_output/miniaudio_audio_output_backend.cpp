#include "infrastructure/audio_output/miniaudio_audio_output_backend.hpp"

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio/miniaudio.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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
    ~Impl() { unconfigure(); }

    void set_progress_notifier(
        contracts::audio_output::AudioOutputBackendProgressNotifier* notifier) noexcept {
        std::lock_guard lock(mutex);
        progress_notifier = notifier;
    }

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
        buffer.assign(playback_format.sample_rate * playback_format.channels * kBytesPerSample *
                          kRingBufferSeconds,
                      std::byte{0});
        read_offset = 0;
        write_offset = 0;
        buffered_bytes = 0;

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

        result = ma_device_start(&device);
        if (result != MA_SUCCESS) {
            ma_device_uninit(&device);
            device_initialized = false;
            buffer.clear();
            return std::unexpected(make_error(
                AudioOutputBackendOperation::Configure,
                static_cast<int>(result),
                "miniaudio device start failed"));
        }

        configured = true;
        notify_progress_available_unlocked();
        return AudioOutputConfigureResult{.playback_format = playback_format};
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
        if (buffer.size() - buffered_bytes < expected_bytes) {
            return AudioOutputSubmitStatus::WouldBlock;
        }

        write_bytes(audio.planes.front().data(), expected_bytes);
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
        return buffered_bytes == 0 ? AudioOutputDrainStatus::Drained
                                   : AudioOutputDrainStatus::WouldBlock;
    }

    void reset() noexcept {
        {
            std::lock_guard lock(mutex);
            read_offset = 0;
            write_offset = 0;
            buffered_bytes = 0;
        }
        notify_progress_available();
    }

    void unconfigure() noexcept {
        {
            std::lock_guard lock(mutex);
            configured = false;
            read_offset = 0;
            write_offset = 0;
            buffered_bytes = 0;
            playback_format = {};
            buffer.clear();
        }

        if (device_initialized) {
            ma_device_uninit(&device);
            device_initialized = false;
        }
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
        const std::size_t requested_bytes =
            static_cast<std::size_t>(frame_count) * kPlaybackChannels * kBytesPerSample;
        auto* bytes = static_cast<std::byte*>(output);

        std::size_t copied = 0;
        contracts::audio_output::AudioOutputBackendProgressNotifier* notifier = nullptr;
        {
            std::lock_guard lock(mutex);
            copied = read_bytes(bytes, requested_bytes);
            notifier = progress_notifier;
        }
        if (copied < requested_bytes) {
            std::memset(bytes + copied, 0, requested_bytes - copied);
        }
        if (copied > 0 && notifier != nullptr) {
            notifier->notify_audio_output_progress_available();
        }
    }

    void notify_progress_available() noexcept {
        contracts::audio_output::AudioOutputBackendProgressNotifier* notifier = nullptr;
        {
            std::lock_guard lock(mutex);
            notifier = progress_notifier;
        }
        if (notifier != nullptr) {
            notifier->notify_audio_output_progress_available();
        }
    }

    void notify_progress_available_unlocked() noexcept {
        if (progress_notifier != nullptr) {
            progress_notifier->notify_audio_output_progress_available();
        }
    }

    void write_bytes(const std::byte* source, std::size_t byte_count) noexcept {
        assert(byte_count <= buffer.size() - buffered_bytes);
        const std::size_t first_chunk = std::min(byte_count, buffer.size() - write_offset);
        std::memcpy(buffer.data() + write_offset, source, first_chunk);
        if (byte_count > first_chunk) {
            std::memcpy(buffer.data(), source + first_chunk, byte_count - first_chunk);
        }
        write_offset = (write_offset + byte_count) % buffer.size();
        buffered_bytes += byte_count;
    }

    std::size_t read_bytes(std::byte* destination, std::size_t byte_count) noexcept {
        const std::size_t bytes_to_read = std::min(byte_count, buffered_bytes);
        if (bytes_to_read == 0 || buffer.empty()) {
            return 0;
        }

        const std::size_t first_chunk = std::min(bytes_to_read, buffer.size() - read_offset);
        std::memcpy(destination, buffer.data() + read_offset, first_chunk);
        if (bytes_to_read > first_chunk) {
            std::memcpy(destination + first_chunk, buffer.data(), bytes_to_read - first_chunk);
        }
        read_offset = (read_offset + bytes_to_read) % buffer.size();
        buffered_bytes -= bytes_to_read;
        return bytes_to_read;
    }

    std::mutex mutex;
    contracts::audio_output::AudioOutputBackendProgressNotifier* progress_notifier = nullptr;
    AudioPcmFormat playback_format{};
    std::vector<std::byte> buffer;
    std::size_t read_offset = 0;
    std::size_t write_offset = 0;
    std::size_t buffered_bytes = 0;
    ma_device device{};
    bool device_initialized = false;
    bool configured = false;
};

MiniaudioAudioOutputBackend::MiniaudioAudioOutputBackend()
    : impl_(std::make_unique<Impl>()) {}

MiniaudioAudioOutputBackend::~MiniaudioAudioOutputBackend() = default;

void MiniaudioAudioOutputBackend::set_progress_notifier(
    contracts::audio_output::AudioOutputBackendProgressNotifier* notifier) noexcept {
    impl_->set_progress_notifier(notifier);
}

std::expected<contracts::audio_output::AudioOutputConfigureResult,
              contracts::audio_output::AudioOutputBackendError>
MiniaudioAudioOutputBackend::configure(const contracts::audio_output::AudioOutputOptions& options) {
    return impl_->configure(options);
}

std::expected<contracts::audio_output::AudioOutputSubmitStatus,
              contracts::audio_output::AudioOutputBackendError>
MiniaudioAudioOutputBackend::try_submit(const contracts::media::DecodedAudio& audio) {
    return impl_->try_submit(audio);
}

std::expected<contracts::audio_output::AudioOutputDrainStatus,
              contracts::audio_output::AudioOutputBackendError>
MiniaudioAudioOutputBackend::try_drain() {
    return impl_->try_drain();
}

void MiniaudioAudioOutputBackend::reset() noexcept {
    impl_->reset();
}

void MiniaudioAudioOutputBackend::unconfigure() noexcept {
    impl_->unconfigure();
}

} // namespace semi::infra::audio_output
