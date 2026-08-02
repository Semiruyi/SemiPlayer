#include "infrastructure/ffmpeg/audio_decoder/ffmpeg_audio_decoder_backend.hpp"
#include "infrastructure/ffmpeg/ffmpeg_raii.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace semi::infra::ffmpeg::audio_decoder {
namespace {

using contracts::audio_decoder::AudioDecoderBackendError;
using contracts::audio_decoder::AudioDecoderBackendConfigureResult;
using contracts::audio_decoder::AudioDecoderBackendOperation;
using contracts::audio_decoder::DecodedAudioBatch;
using contracts::demuxer::packet::EncodedPacket;
using contracts::media::AudioPcmFormat;
using contracts::media::AudioSampleFormat;
using contracts::media::DecodedAudio;

std::string ffmpeg_message(int error_code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(error_code, buffer.data(), buffer.size()) < 0) {
        return "FFmpeg error " + std::to_string(error_code);
    }
    return buffer.data();
}

AudioDecoderBackendError make_error(AudioDecoderBackendOperation operation, int error_code) {
    return AudioDecoderBackendError{
        .operation = operation,
        .native_code = error_code,
        .message = ffmpeg_message(error_code),
    };
}

AudioDecoderBackendError make_state_error(AudioDecoderBackendOperation operation,
                                          const char* message) {
    return AudioDecoderBackendError{
        .operation = operation,
        .native_code = AVERROR(EINVAL),
        .message = message,
    };
}

std::optional<AudioSampleFormat> sample_format(AVSampleFormat format) noexcept {
    switch (format) {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_U8P:
        return AudioSampleFormat::U8;
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
        return AudioSampleFormat::S16;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
        return AudioSampleFormat::S32;
    case AV_SAMPLE_FMT_S64:
    case AV_SAMPLE_FMT_S64P:
        return AudioSampleFormat::S64;
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP:
        return AudioSampleFormat::F32;
    case AV_SAMPLE_FMT_DBL:
    case AV_SAMPLE_FMT_DBLP:
        return AudioSampleFormat::F64;
    default:
        return std::nullopt;
    }
}

std::optional<std::int64_t> timestamp_us(const AVFrame& frame) noexcept {
    if (frame.best_effort_timestamp == AV_NOPTS_VALUE) {
        return std::nullopt;
    }
    return frame.best_effort_timestamp;
}

std::expected<AudioPcmFormat, AudioDecoderBackendError>
decoded_format_from_context(const AVCodecContext& context) {
    if (context.sample_rate <= 0 || context.ch_layout.nb_channels <= 0 ||
        context.sample_fmt == AV_SAMPLE_FMT_NONE) {
        return std::unexpected(make_state_error(AudioDecoderBackendOperation::Configure,
                                                "FFmpeg audio decoder did not expose a PCM output format"));
    }

    const auto native_format = static_cast<AVSampleFormat>(context.sample_fmt);
    const auto contract_format = sample_format(native_format);
    if (!contract_format || av_get_bytes_per_sample(native_format) <= 0) {
        return std::unexpected(make_state_error(AudioDecoderBackendOperation::Configure,
                                                "FFmpeg audio decoder output sample format is unsupported"));
    }

    return AudioPcmFormat{
        .sample_rate = static_cast<std::uint32_t>(context.sample_rate),
        .channels = static_cast<std::uint32_t>(context.ch_layout.nb_channels),
        .sample_format = *contract_format,
        .planar = av_sample_fmt_is_planar(native_format) != 0,
    };
}

std::expected<DecodedAudio, AudioDecoderBackendError>
copy_frame(const AVFrame& frame, AudioDecoderBackendOperation operation) {
    if (frame.nb_samples < 0 || frame.sample_rate <= 0 || frame.ch_layout.nb_channels <= 0) {
        return std::unexpected(make_state_error(operation, "FFmpeg frame has invalid audio dimensions"));
    }

    const auto native_format = static_cast<AVSampleFormat>(frame.format);
    const auto contract_format = sample_format(native_format);
    const int bytes_per_sample = av_get_bytes_per_sample(native_format);
    if (!contract_format || bytes_per_sample <= 0) {
        return std::unexpected(make_state_error(operation, "FFmpeg frame has an unsupported sample format"));
    }

    const auto samples = static_cast<std::size_t>(frame.nb_samples);
    const auto channels = static_cast<std::size_t>(frame.ch_layout.nb_channels);
    const auto bytes = static_cast<std::size_t>(bytes_per_sample);
    if (samples > std::numeric_limits<std::size_t>::max() / bytes ||
        (!av_sample_fmt_is_planar(native_format) &&
         samples > std::numeric_limits<std::size_t>::max() / (channels * bytes))) {
        return std::unexpected(make_state_error(operation, "FFmpeg frame PCM size overflows"));
    }

    const bool planar = av_sample_fmt_is_planar(native_format) != 0;
    const std::size_t plane_count = planar ? channels : 1U;
    const std::size_t plane_size = planar ? samples * bytes : samples * channels * bytes;
    if (frame.extended_data == nullptr) {
        return std::unexpected(make_state_error(operation, "FFmpeg frame has no PCM data"));
    }

    try {
        DecodedAudio result{
            .format = AudioPcmFormat{
                .sample_rate = static_cast<std::uint32_t>(frame.sample_rate),
                .channels = static_cast<std::uint32_t>(frame.ch_layout.nb_channels),
                .sample_format = *contract_format,
                .planar = planar,
            },
            .samples_per_channel = static_cast<std::uint32_t>(frame.nb_samples),
            .planes = std::vector<std::vector<std::byte>>(plane_count),
            .pts_us = timestamp_us(frame),
        };
        for (std::size_t index = 0; index < plane_count; ++index) {
            if (frame.extended_data[index] == nullptr && plane_size != 0) {
                return std::unexpected(make_state_error(operation, "FFmpeg frame has a missing PCM plane"));
            }
            auto& destination = result.planes[index];
            destination.resize(plane_size);
            if (plane_size != 0) {
                std::memcpy(destination.data(), frame.extended_data[index], plane_size);
            }
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(operation, AVERROR(ENOMEM)));
    }
}

std::expected<void, AudioDecoderBackendError>
append_received_frames(AVCodecContext& context, AVFrame& frame, DecodedAudioBatch& output,
                       AudioDecoderBackendOperation operation) {
    for (;;) {
        const int status = avcodec_receive_frame(&context, &frame);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
            return {};
        }
        if (status < 0) {
            return std::unexpected(make_error(operation, status));
        }

        auto copied = copy_frame(frame, operation);
        av_frame_unref(&frame);
        if (!copied) {
            return std::unexpected(std::move(copied.error()));
        }
        output.push_back(std::move(*copied));
    }
}

std::expected<DecodedAudioBatch, AudioDecoderBackendError>
send_packet_and_collect_frames(AVCodecContext& context, AVFrame& frame, AVPacket* packet,
                               AudioDecoderBackendOperation operation) {
    try {
        DecodedAudioBatch output;
        for (;;) {
            const int status = avcodec_send_packet(&context, packet);
            if (status == AVERROR(EAGAIN)) {
                // A prior packet still has unread output. Drain it before retrying this packet.
                auto received = append_received_frames(context, frame, output, operation);
                if (!received) {
                    return std::unexpected(std::move(received.error()));
                }
                continue;
            }

            if (packet != nullptr) {
                av_packet_unref(packet);
            }
            if (status == AVERROR_EOF && packet == nullptr) {
                return output;
            }
            if (status < 0) {
                return std::unexpected(make_error(operation, status));
            }
            break;
        }

        // The packet was accepted; collect every frame it produced.
        auto received = append_received_frames(context, frame, output, operation);
        if (!received) {
            return std::unexpected(std::move(received.error()));
        }
        return output;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(operation, AVERROR(ENOMEM)));
    }
}

std::expected<void, AudioDecoderBackendError>
prepare_packet(AVPacket& destination, const EncodedPacket& source,
               AudioDecoderBackendOperation operation) {
    if (source.payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(make_state_error(operation, "encoded packet payload is too large for FFmpeg"));
    }

    av_packet_unref(&destination);
    const int status = av_new_packet(&destination, static_cast<int>(source.payload.size()));
    if (status < 0) {
        return std::unexpected(make_error(operation, status));
    }
    if (!source.payload.empty()) {
        std::memcpy(destination.data, source.payload.data(), source.payload.size());
    }
    destination.pts = source.pts_us.value_or(AV_NOPTS_VALUE);
    destination.dts = source.dts_us.value_or(AV_NOPTS_VALUE);
    destination.duration = source.duration_us.value_or(0);
    return {};
}

} // namespace

struct FfmpegAudioDecoderBackend::Impl {
    AvCodecContextPtr context;
    AvPacketPtr packet;
    AvFramePtr frame;
    bool draining = false;
};

FfmpegAudioDecoderBackend::FfmpegAudioDecoderBackend() : impl_(std::make_unique<Impl>()) {}

FfmpegAudioDecoderBackend::~FfmpegAudioDecoderBackend() {
    unconfigure();
}

std::expected<AudioDecoderBackendConfigureResult, AudioDecoderBackendError>
FfmpegAudioDecoderBackend::configure(const contracts::media::AudioCodecConfig& config) {
    if (impl_->context != nullptr) {
        return std::unexpected(make_state_error(AudioDecoderBackendOperation::Configure,
                                                "FFmpeg audio decoder backend is already configured"));
    }
    if (config.common.codec_name.empty() || config.sample_rate == 0 || config.channels == 0 ||
        config.sample_rate > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        config.channels > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(make_state_error(AudioDecoderBackendOperation::Configure,
                                                "audio decoder configuration is incomplete"));
    }

    const AVCodec* codec = avcodec_find_decoder_by_name(config.common.codec_name.c_str());
    if (codec == nullptr || codec->type != AVMEDIA_TYPE_AUDIO) {
        return std::unexpected(make_state_error(AudioDecoderBackendOperation::Configure,
                                                "FFmpeg could not find the requested audio decoder"));
    }

    AvCodecContextPtr context(avcodec_alloc_context3(codec));
    AvPacketPtr packet(av_packet_alloc());
    AvFramePtr frame(av_frame_alloc());
    if (context == nullptr || packet == nullptr || frame == nullptr) {
        return std::unexpected(make_error(AudioDecoderBackendOperation::Configure, AVERROR(ENOMEM)));
    }

    context->sample_rate = static_cast<int>(config.sample_rate);
    av_channel_layout_default(&context->ch_layout, static_cast<int>(config.channels));
    context->pkt_timebase = AV_TIME_BASE_Q;
    if (!config.common.extradata.empty()) {
        if (config.common.extradata.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE)) {
            return std::unexpected(make_state_error(AudioDecoderBackendOperation::Configure,
                                                    "audio decoder extradata is too large for FFmpeg"));
        }
        context->extradata_size = static_cast<int>(config.common.extradata.size());
        context->extradata = static_cast<std::uint8_t*>(
            av_mallocz(config.common.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (context->extradata == nullptr) {
            return std::unexpected(make_error(AudioDecoderBackendOperation::Configure, AVERROR(ENOMEM)));
        }
        std::memcpy(context->extradata, config.common.extradata.data(), config.common.extradata.size());
    }

    const int status = avcodec_open2(context.get(), codec, nullptr);
    if (status < 0) {
        return std::unexpected(make_error(AudioDecoderBackendOperation::Configure, status));
    }

    auto decoded_format = decoded_format_from_context(*context);
    if (!decoded_format) {
        return std::unexpected(std::move(decoded_format.error()));
    }

    impl_->context = std::move(context);
    impl_->packet = std::move(packet);
    impl_->frame = std::move(frame);
    impl_->draining = false;
    return AudioDecoderBackendConfigureResult{
        .decoded_format = *decoded_format,
    };
}

std::expected<DecodedAudioBatch, AudioDecoderBackendError>
FfmpegAudioDecoderBackend::decode(const EncodedPacket& packet) {
    if (impl_->context == nullptr) {
        return std::unexpected(make_state_error(AudioDecoderBackendOperation::Decode,
                                                "FFmpeg audio decoder backend is not configured"));
    }
    if (impl_->draining) {
        return std::unexpected(make_state_error(AudioDecoderBackendOperation::Decode,
                                                "FFmpeg audio decoder must be reset after drain"));
    }

    auto prepared = prepare_packet(*impl_->packet, packet, AudioDecoderBackendOperation::Decode);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    return send_packet_and_collect_frames(*impl_->context, *impl_->frame, impl_->packet.get(),
                                          AudioDecoderBackendOperation::Decode);
}

std::expected<DecodedAudioBatch, AudioDecoderBackendError> FfmpegAudioDecoderBackend::drain() {
    if (impl_->context == nullptr) {
        return std::unexpected(make_state_error(AudioDecoderBackendOperation::Drain,
                                                "FFmpeg audio decoder backend is not configured"));
    }
    if (impl_->draining) {
        return DecodedAudioBatch{};
    }

    auto output = send_packet_and_collect_frames(*impl_->context, *impl_->frame, nullptr,
                                                  AudioDecoderBackendOperation::Drain);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    impl_->draining = true;
    return output;
}

void FfmpegAudioDecoderBackend::reset() noexcept {
    if (impl_->context != nullptr) {
        avcodec_flush_buffers(impl_->context.get());
        impl_->draining = false;
    }
}

void FfmpegAudioDecoderBackend::unconfigure() noexcept {
    if (!impl_) {
        return;
    }
    impl_->frame.reset();
    impl_->packet.reset();
    impl_->context.reset();
    impl_->draining = false;
}

} // namespace semi::infra::ffmpeg::audio_decoder
