#include "infrastructure/ffmpeg/ffmpeg_demuxer_backend.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace semi::infra::ffmpeg {
namespace {

using domain::AudioCodecConfig;
using domain::BackendProbeResult;
using domain::BackendStreamId;
using domain::CodecCommon;
using domain::DemuxerBackendError;
using domain::DemuxerBackendOperation;
using domain::OtherStreamConfig;
using domain::OtherStreamKind;
using domain::Rational;
using domain::StreamDescriptor;
using domain::StreamTiming;
using domain::SubtitleCodecConfig;
using domain::VideoCodecConfig;

std::string ffmpeg_message(int error_code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(error_code, buffer.data(), buffer.size()) < 0) {
        return "FFmpeg error " + std::to_string(error_code);
    }
    return buffer.data();
}

DemuxerBackendError make_error(DemuxerBackendOperation operation, int error_code) {
    return DemuxerBackendError{
        .operation = operation,
        .native_code = error_code,
        .message = ffmpeg_message(error_code),
    };
}

std::vector<std::byte> copy_extradata(const AVCodecParameters& parameters) {
    if (parameters.extradata == nullptr || parameters.extradata_size <= 0) {
        return {};
    }
    const auto size = static_cast<std::size_t>(parameters.extradata_size);
    const auto* bytes = reinterpret_cast<const std::byte*>(parameters.extradata);
    return {bytes, bytes + size};
}

CodecCommon make_common(const AVCodecParameters& parameters) {
    const char* codec_name = avcodec_get_name(parameters.codec_id);
    return CodecCommon{
        .codec_name = codec_name != nullptr ? codec_name : "unknown",
        .extradata = copy_extradata(parameters),
    };
}

std::optional<std::int64_t> optional_timestamp(std::int64_t value) {
    return value == AV_NOPTS_VALUE ? std::nullopt : std::optional{value};
}

OtherStreamKind other_kind(AVMediaType type) {
    switch (type) {
    case AVMEDIA_TYPE_DATA:
        return OtherStreamKind::Data;
    case AVMEDIA_TYPE_ATTACHMENT:
        return OtherStreamKind::Attachment;
    default:
        return OtherStreamKind::Unknown;
    }
}

StreamDescriptor make_stream_descriptor(const AVStream& stream) {
    const AVCodecParameters& parameters = *stream.codecpar;
    StreamDescriptor descriptor;
    descriptor.id = BackendStreamId{static_cast<std::uint32_t>(stream.index)};
    descriptor.timing = StreamTiming{
        .time_base = Rational{stream.time_base.num, stream.time_base.den},
        .start_pts = optional_timestamp(stream.start_time),
        .duration_pts = optional_timestamp(stream.duration),
    };

    switch (parameters.codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        descriptor.config = VideoCodecConfig{
            .common = make_common(parameters),
            .coded_width = parameters.width > 0 ? static_cast<std::uint32_t>(parameters.width) : 0U,
            .coded_height = parameters.height > 0 ? static_cast<std::uint32_t>(parameters.height) : 0U,
            .profile = parameters.profile == AV_PROFILE_UNKNOWN
                ? std::nullopt
                : std::optional{parameters.profile},
            .level = parameters.level == AV_LEVEL_UNKNOWN ? std::nullopt : std::optional{parameters.level},
        };
        break;
    case AVMEDIA_TYPE_AUDIO:
        descriptor.config = AudioCodecConfig{
            .common = make_common(parameters),
            .sample_rate = parameters.sample_rate > 0 ? static_cast<std::uint32_t>(parameters.sample_rate) : 0U,
            .channels = parameters.ch_layout.nb_channels > 0
                ? static_cast<std::uint32_t>(parameters.ch_layout.nb_channels)
                : 0U,
        };
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        descriptor.config = SubtitleCodecConfig{.common = make_common(parameters)};
        break;
    default:
        descriptor.config = OtherStreamConfig{
            .common = make_common(parameters),
            .kind = other_kind(parameters.codec_type),
        };
        break;
    }
    return descriptor;
}

} // namespace

struct FfmpegDemuxerBackend::Impl {
    AVFormatContext* format_context = nullptr;
};

FfmpegDemuxerBackend::FfmpegDemuxerBackend() : impl_(std::make_unique<Impl>()) {}

FfmpegDemuxerBackend::~FfmpegDemuxerBackend() {
    close();
}

std::expected<BackendProbeResult, DemuxerBackendError>
FfmpegDemuxerBackend::open(std::string_view source) {
    if (impl_->format_context != nullptr) {
        return std::unexpected(DemuxerBackendError{
            .operation = DemuxerBackendOperation::Open,
            .message = "FFmpeg demuxer backend is already open",
        });
    }

    const std::string source_copy(source);
    AVFormatContext* context = nullptr;
    int status = avformat_open_input(&context, source_copy.c_str(), nullptr, nullptr);
    if (status < 0) {
        return std::unexpected(make_error(DemuxerBackendOperation::Open, status));
    }

    status = avformat_find_stream_info(context, nullptr);
    if (status < 0) {
        avformat_close_input(&context);
        return std::unexpected(make_error(DemuxerBackendOperation::Probe, status));
    }

    BackendProbeResult result;
    if (context->duration != AV_NOPTS_VALUE) {
        result.container.duration_us = context->duration;
    }
    result.streams.reserve(context->nb_streams);
    for (unsigned int index = 0; index < context->nb_streams; ++index) {
        result.streams.push_back(make_stream_descriptor(*context->streams[index]));
    }

    impl_->format_context = context;
    return result;
}

void FfmpegDemuxerBackend::close() noexcept {
    if (impl_ && impl_->format_context != nullptr) {
        avformat_close_input(&impl_->format_context);
    }
}

} // namespace semi::infra::ffmpeg
