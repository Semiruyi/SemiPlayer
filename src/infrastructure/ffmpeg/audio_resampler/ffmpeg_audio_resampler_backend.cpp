#include "infrastructure/ffmpeg/audio_resampler/ffmpeg_audio_resampler_backend.hpp"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace semi::infra::ffmpeg::audio_resampler {
namespace {

using contracts::audio_resampler::AudioResamplerBackendError;
using contracts::audio_resampler::AudioResamplerBackendOperation;
using contracts::audio_resampler::ResampledAudioBatch;
using contracts::media::AudioPcmFormat;
using contracts::media::AudioSampleFormat;
using contracts::media::DecodedAudio;

struct SwrContextDeleter {
    void operator()(SwrContext* context) const noexcept {
        swr_free(&context);
    }
};

using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

std::string ffmpeg_message(int error_code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(error_code, buffer.data(), buffer.size()) < 0) {
        return "FFmpeg error " + std::to_string(error_code);
    }
    return buffer.data();
}

AudioResamplerBackendError make_error(AudioResamplerBackendOperation operation, int error_code) {
    return AudioResamplerBackendError{
        .operation = operation,
        .native_code = error_code,
        .message = ffmpeg_message(error_code),
    };
}

AudioResamplerBackendError make_state_error(AudioResamplerBackendOperation operation,
                                            const char* message) {
    return AudioResamplerBackendError{
        .operation = operation,
        .native_code = AVERROR(EINVAL),
        .message = message,
    };
}

std::optional<AVSampleFormat> native_sample_format(const AudioPcmFormat& format) noexcept {
    switch (format.sample_format) {
    case AudioSampleFormat::U8:
        return format.planar ? AV_SAMPLE_FMT_U8P : AV_SAMPLE_FMT_U8;
    case AudioSampleFormat::S16:
        return format.planar ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S16;
    case AudioSampleFormat::S32:
        return format.planar ? AV_SAMPLE_FMT_S32P : AV_SAMPLE_FMT_S32;
    case AudioSampleFormat::S64:
        return format.planar ? AV_SAMPLE_FMT_S64P : AV_SAMPLE_FMT_S64;
    case AudioSampleFormat::F32:
        return format.planar ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT;
    case AudioSampleFormat::F64:
        return format.planar ? AV_SAMPLE_FMT_DBLP : AV_SAMPLE_FMT_DBL;
    case AudioSampleFormat::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

bool valid_format(const AudioPcmFormat& format) noexcept {
    return format.sample_rate > 0 && format.channels > 0 &&
           format.sample_rate <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
           format.channels <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
           native_sample_format(format).has_value();
}

std::optional<std::size_t> plane_size_bytes(const AudioPcmFormat& format,
                                            std::uint32_t samples_per_channel) noexcept {
    const auto native_format = native_sample_format(format);
    if (!native_format) {
        return std::nullopt;
    }

    const int bytes_per_sample = av_get_bytes_per_sample(*native_format);
    if (bytes_per_sample <= 0) {
        return std::nullopt;
    }

    const auto samples = static_cast<std::size_t>(samples_per_channel);
    const auto bytes = static_cast<std::size_t>(bytes_per_sample);
    if (format.planar) {
        if (samples > std::numeric_limits<std::size_t>::max() / bytes) {
            return std::nullopt;
        }
        return samples * bytes;
    }

    const auto channels = static_cast<std::size_t>(format.channels);
    if (channels != 0 && samples > std::numeric_limits<std::size_t>::max() / channels) {
        return std::nullopt;
    }
    const auto interleaved_samples = samples * channels;
    if (interleaved_samples > std::numeric_limits<std::size_t>::max() / bytes) {
        return std::nullopt;
    }
    return interleaved_samples * bytes;
}

std::expected<std::vector<const std::uint8_t*>, AudioResamplerBackendError>
input_planes(const DecodedAudio& input, AudioResamplerBackendOperation operation) {
    if (!valid_format(input.format) || input.samples_per_channel == 0) {
        return std::unexpected(make_state_error(operation, "input PCM format is invalid"));
    }

    const std::size_t expected_planes = input.format.planar ? input.format.channels : 1U;
    if (input.planes.size() != expected_planes) {
        return std::unexpected(make_state_error(operation, "input PCM plane count is invalid"));
    }

    const auto expected_size = plane_size_bytes(input.format, input.samples_per_channel);
    if (!expected_size) {
        return std::unexpected(make_state_error(operation, "input PCM size overflows"));
    }

    std::vector<const std::uint8_t*> planes;
    planes.reserve(input.planes.size());
    for (const auto& plane : input.planes) {
        if (plane.size() != *expected_size) {
            return std::unexpected(make_state_error(operation, "input PCM plane size is invalid"));
        }
        planes.push_back(reinterpret_cast<const std::uint8_t*>(plane.data()));
    }
    return planes;
}

std::expected<DecodedAudio, AudioResamplerBackendError>
make_output_frame(const AudioPcmFormat& output_format,
                  std::uint32_t samples_per_channel,
                  std::optional<std::int64_t> pts_us,
                  AudioResamplerBackendOperation operation) {
    const auto plane_size = plane_size_bytes(output_format, samples_per_channel);
    if (!plane_size) {
        return std::unexpected(make_state_error(operation, "output PCM size overflows"));
    }

    const std::size_t plane_count = output_format.planar ? output_format.channels : 1U;
    try {
        return DecodedAudio{
            .format = output_format,
            .samples_per_channel = samples_per_channel,
            .planes = std::vector<std::vector<std::byte>>(plane_count,
                                                          std::vector<std::byte>(*plane_size)),
            .pts_us = pts_us,
        };
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(operation, AVERROR(ENOMEM)));
    }
}

std::vector<std::uint8_t*> mutable_planes(DecodedAudio& audio) {
    std::vector<std::uint8_t*> planes;
    planes.reserve(audio.planes.size());
    for (auto& plane : audio.planes) {
        planes.push_back(reinterpret_cast<std::uint8_t*>(plane.data()));
    }
    return planes;
}

void trim_output_planes(DecodedAudio& audio, std::uint32_t actual_samples) {
    audio.samples_per_channel = actual_samples;
    const auto size = plane_size_bytes(audio.format, actual_samples);
    if (!size) {
        return;
    }
    for (auto& plane : audio.planes) {
        plane.resize(*size);
    }
}

std::expected<std::optional<DecodedAudio>, AudioResamplerBackendError>
convert_samples(SwrContext& context,
                const AudioPcmFormat& input_format,
                const AudioPcmFormat& output_format,
                const std::uint8_t* const* input_data,
                int input_samples,
                std::optional<std::int64_t> pts_us,
                AudioResamplerBackendOperation operation) {
    const auto delay = swr_get_delay(&context, static_cast<int>(input_format.sample_rate));
    if (delay < 0) {
        return std::unexpected(make_error(operation, static_cast<int>(delay)));
    }

    const auto requested_samples = av_rescale_rnd(delay + input_samples,
                                                  static_cast<int>(output_format.sample_rate),
                                                  static_cast<int>(input_format.sample_rate),
                                                  AV_ROUND_UP);
    if (requested_samples < 0 ||
        requested_samples > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(make_state_error(operation, "output sample count overflows"));
    }
    if (requested_samples == 0) {
        return std::nullopt;
    }

    auto output = make_output_frame(output_format,
                                    static_cast<std::uint32_t>(requested_samples),
                                    pts_us,
                                    operation);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    auto output_data = mutable_planes(*output);

    const int converted = swr_convert(&context,
                                      output_data.data(),
                                      static_cast<int>(requested_samples),
                                      input_data,
                                      input_samples);
    if (converted < 0) {
        return std::unexpected(make_error(operation, converted));
    }
    if (converted == 0) {
        return std::nullopt;
    }

    trim_output_planes(*output, static_cast<std::uint32_t>(converted));
    return std::optional<DecodedAudio>{std::move(*output)};
}

} // namespace

struct FfmpegAudioResamplerBackend::Impl {
    SwrContextPtr context;
    AudioPcmFormat input_format;
    AudioPcmFormat output_format;
    bool draining = false;
};

FfmpegAudioResamplerBackend::FfmpegAudioResamplerBackend() : impl_(std::make_unique<Impl>()) {}

FfmpegAudioResamplerBackend::~FfmpegAudioResamplerBackend() {
    unconfigure();
}

std::expected<void, AudioResamplerBackendError>
FfmpegAudioResamplerBackend::configure(const AudioPcmFormat& input_format,
                                       const AudioPcmFormat& output_format) {
    if (impl_->context != nullptr) {
        return std::unexpected(make_state_error(AudioResamplerBackendOperation::Configure,
                                                "FFmpeg audio resampler backend is already configured"));
    }
    if (!valid_format(input_format) || !valid_format(output_format)) {
        return std::unexpected(make_state_error(AudioResamplerBackendOperation::Configure,
                                                "audio resampler configuration is incomplete"));
    }

    AVChannelLayout input_layout{};
    AVChannelLayout output_layout{};
    av_channel_layout_default(&input_layout, static_cast<int>(input_format.channels));
    av_channel_layout_default(&output_layout, static_cast<int>(output_format.channels));

    SwrContext* raw_context = nullptr;
    const int allocated = swr_alloc_set_opts2(&raw_context,
                                              &output_layout,
                                              *native_sample_format(output_format),
                                              static_cast<int>(output_format.sample_rate),
                                              &input_layout,
                                              *native_sample_format(input_format),
                                              static_cast<int>(input_format.sample_rate),
                                              0,
                                              nullptr);
    av_channel_layout_uninit(&input_layout);
    av_channel_layout_uninit(&output_layout);
    if (allocated < 0) {
        return std::unexpected(make_error(AudioResamplerBackendOperation::Configure, allocated));
    }
    SwrContextPtr context(raw_context);
    if (context == nullptr) {
        return std::unexpected(make_error(AudioResamplerBackendOperation::Configure, AVERROR(ENOMEM)));
    }

    const int initialized = swr_init(context.get());
    if (initialized < 0) {
        return std::unexpected(make_error(AudioResamplerBackendOperation::Configure, initialized));
    }

    impl_->context = std::move(context);
    impl_->input_format = input_format;
    impl_->output_format = output_format;
    impl_->draining = false;
    return {};
}

std::expected<ResampledAudioBatch, AudioResamplerBackendError>
FfmpegAudioResamplerBackend::resample(const DecodedAudio& input) {
    if (impl_->context == nullptr) {
        return std::unexpected(make_state_error(AudioResamplerBackendOperation::Resample,
                                                "FFmpeg audio resampler backend is not configured"));
    }
    if (impl_->draining) {
        return std::unexpected(make_state_error(AudioResamplerBackendOperation::Resample,
                                                "FFmpeg audio resampler must be reset after drain"));
    }
    if (input.format.sample_rate != impl_->input_format.sample_rate ||
        input.format.channels != impl_->input_format.channels ||
        input.format.sample_format != impl_->input_format.sample_format ||
        input.format.planar != impl_->input_format.planar) {
        return std::unexpected(make_state_error(AudioResamplerBackendOperation::Resample,
                                                "input PCM format does not match configuration"));
    }

    auto planes = input_planes(input, AudioResamplerBackendOperation::Resample);
    if (!planes) {
        return std::unexpected(std::move(planes.error()));
    }

    auto converted = convert_samples(*impl_->context,
                                     impl_->input_format,
                                     impl_->output_format,
                                     planes->data(),
                                     static_cast<int>(input.samples_per_channel),
                                     input.pts_us,
                                     AudioResamplerBackendOperation::Resample);
    if (!converted) {
        return std::unexpected(std::move(converted.error()));
    }

    ResampledAudioBatch output;
    if (*converted) {
        try {
            output.push_back(std::move(**converted));
        } catch (const std::bad_alloc&) {
            return std::unexpected(make_error(AudioResamplerBackendOperation::Resample, AVERROR(ENOMEM)));
        }
    }
    return output;
}

std::expected<ResampledAudioBatch, AudioResamplerBackendError>
FfmpegAudioResamplerBackend::drain() {
    if (impl_->context == nullptr) {
        return std::unexpected(make_state_error(AudioResamplerBackendOperation::Drain,
                                                "FFmpeg audio resampler backend is not configured"));
    }
    if (impl_->draining) {
        return ResampledAudioBatch{};
    }

    try {
        ResampledAudioBatch output;
        for (;;) {
            auto converted = convert_samples(*impl_->context,
                                             impl_->input_format,
                                             impl_->output_format,
                                             nullptr,
                                             0,
                                             std::nullopt,
                                             AudioResamplerBackendOperation::Drain);
            if (!converted) {
                return std::unexpected(std::move(converted.error()));
            }
            if (!*converted) {
                break;
            }
            output.push_back(std::move(**converted));
        }
        impl_->draining = true;
        return output;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(AudioResamplerBackendOperation::Drain, AVERROR(ENOMEM)));
    }
}

void FfmpegAudioResamplerBackend::reset() noexcept {
    if (impl_->context != nullptr) {
        swr_close(impl_->context.get());
        (void)swr_init(impl_->context.get());
        impl_->draining = false;
    }
}

void FfmpegAudioResamplerBackend::unconfigure() noexcept {
    if (!impl_) {
        return;
    }
    impl_->context.reset();
    impl_->input_format = {};
    impl_->output_format = {};
    impl_->draining = false;
}

} // namespace semi::infra::ffmpeg::audio_resampler
