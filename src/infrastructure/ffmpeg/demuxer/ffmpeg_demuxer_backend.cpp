#include "infrastructure/ffmpeg/demuxer/ffmpeg_demuxer_backend.hpp"
#include "infrastructure/ffmpeg/ffmpeg_raii.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

namespace semi::infra::ffmpeg::demuxer {
namespace {

using contracts::demuxer::BackendProbeResult;
using contracts::demuxer::BackendReadResult;
using contracts::demuxer::DemuxerBackendError;
using contracts::demuxer::DemuxerBackendOperation;
using contracts::demuxer::packet::EncodedPacket;
using contracts::media::AudioCodecConfig;
using contracts::media::CodecCommon;
using contracts::media::OtherStreamConfig;
using contracts::media::OtherStreamKind;
using contracts::media::StreamDescriptor;
using contracts::media::StreamTiming;
using contracts::media::SubtitleCodecConfig;
using contracts::media::TimeBase;
using contracts::media::VideoCodecConfig;

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

bool valid_time_base(AVRational time_base) noexcept {
    return time_base.num > 0 && time_base.den > 0;
}

std::optional<std::int64_t> rescale_timestamp(std::int64_t value, AVRational time_base) {
    if (value == AV_NOPTS_VALUE) {
        return std::nullopt;
    }
    return av_rescale_q(value, time_base, AV_TIME_BASE_Q);
}

std::optional<std::int64_t> rescale_duration(std::int64_t value, AVRational time_base) {
    if (value == AV_NOPTS_VALUE || value == 0) {
        return std::nullopt;
    }
    return av_rescale_q(value, time_base, AV_TIME_BASE_Q);
}

std::expected<EncodedPacket, DemuxerBackendError>
copy_packet(const AVPacket& packet, AVRational time_base) {
    if (!valid_time_base(time_base)) {
        return std::unexpected(DemuxerBackendError{
            .operation = DemuxerBackendOperation::Read,
            .native_code = AVERROR(EINVAL),
            .message = "FFmpeg packet stream has an invalid time base",
        });
    }
    if (packet.size < 0 || (packet.size > 0 && packet.data == nullptr)) {
        return std::unexpected(DemuxerBackendError{
            .operation = DemuxerBackendOperation::Read,
            .native_code = AVERROR_INVALIDDATA,
            .message = "FFmpeg packet has invalid payload data",
        });
    }

    try {
        EncodedPacket result{
            .payload = {},
            .pts_us = rescale_timestamp(packet.pts, time_base),
            .dts_us = rescale_timestamp(packet.dts, time_base),
            .duration_us = rescale_duration(packet.duration, time_base),
        };
        if (packet.size > 0) {
            const auto* begin = reinterpret_cast<const std::byte*>(packet.data);
            result.payload.assign(begin, begin + packet.size);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(DemuxerBackendError{
            .operation = DemuxerBackendOperation::Read,
            .native_code = AVERROR(ENOMEM),
            .message = "failed to copy FFmpeg packet payload",
        });
    }
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
    descriptor.id = contracts::media::DemuxerStreamId{static_cast<std::uint32_t>(stream.index)};
    descriptor.timing = StreamTiming{
        .time_base = TimeBase{stream.time_base.num, stream.time_base.den},
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
    AvFormatInputContextPtr format_context;
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
    AVFormatContext* raw_context = nullptr;
    int status = avformat_open_input(&raw_context, source_copy.c_str(), nullptr, nullptr);
    if (status < 0) {
        return std::unexpected(make_error(DemuxerBackendOperation::Open, status));
    }
    AvFormatInputContextPtr context(raw_context);

    status = avformat_find_stream_info(context.get(), nullptr);
    if (status < 0) {
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

    impl_->format_context = std::move(context);
    return result;
}

std::expected<BackendReadResult, DemuxerBackendError>
FfmpegDemuxerBackend::read_packet() {
    if (impl_->format_context == nullptr) {
        return std::unexpected(DemuxerBackendError{
            .operation = DemuxerBackendOperation::Read,
            .message = "FFmpeg demuxer backend is not open",
        });
    }

    AvPacketPtr packet(av_packet_alloc());
    if (packet == nullptr) {
        return std::unexpected(DemuxerBackendError{
            .operation = DemuxerBackendOperation::Read,
            .native_code = AVERROR(ENOMEM),
            .message = "failed to allocate FFmpeg packet",
        });
    }

    const int status = av_read_frame(impl_->format_context.get(), packet.get());
    if (status == AVERROR_EOF) {
        return contracts::demuxer::BackendEndOfStream{};
    }
    if (status < 0) {
        return std::unexpected(make_error(DemuxerBackendOperation::Read, status));
    }

    if (packet->stream_index < 0 ||
        static_cast<unsigned int>(packet->stream_index) >= impl_->format_context->nb_streams) {
        return std::unexpected(DemuxerBackendError{
            .operation = DemuxerBackendOperation::Read,
            .native_code = AVERROR_INVALIDDATA,
            .message = "FFmpeg packet contains an invalid stream index",
        });
    }

    const auto stream_id = contracts::media::DemuxerStreamId{
        static_cast<std::uint32_t>(packet->stream_index)};
    const AVStream& stream = *impl_->format_context->streams[packet->stream_index];
    auto encoded = copy_packet(*packet, stream.time_base);
    if (!encoded) {
        return std::unexpected(std::move(encoded.error()));
    }

    contracts::demuxer::BackendPacket result{
        .stream_id = stream_id,
        .packet = std::move(*encoded),
    };
    return result;
}

void FfmpegDemuxerBackend::close() noexcept {
    if (impl_) {
        impl_->format_context.reset();
    }
}

} // namespace semi::infra::ffmpeg::demuxer
