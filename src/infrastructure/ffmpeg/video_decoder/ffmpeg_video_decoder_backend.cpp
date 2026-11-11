#include "infrastructure/ffmpeg/video_decoder/ffmpeg_video_decoder_backend.hpp"

#include "infrastructure/ffmpeg/ffmpeg_raii.hpp"
#include "infrastructure/ffmpeg/hardware_decode/ffmpeg_hardware_decode.hpp"
#include "infrastructure/ffmpeg/video_decoder/ffmpeg_video_frame_buffer.hpp"
#include "infrastructure/log/log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#define SEMI_LOG_TAG "ffmpeg_video_decode"

namespace semi::infra::ffmpeg::video_decoder {
namespace {

using contracts::demuxer::packet::EncodedPacket;
using contracts::media::DecodedVideo;
using contracts::video_decoder::DecodedVideoBatch;
using contracts::video_decoder::VideoDecoderBackendError;
using contracts::video_decoder::VideoDecoderBackendOperation;
using hardware_decode::FfmpegHardwareDecodeSession;

AVPixelFormat select_format(AVCodecContext* context, const AVPixelFormat* candidates) noexcept {
    if (!candidates) return AV_PIX_FMT_NONE;
    auto* hardware = static_cast<FfmpegHardwareDecodeSession*>(context->opaque);
    if (hardware) {
        const auto selected = hardware->select_format(*context, candidates);
        if (selected != AV_PIX_FMT_NONE) {
            // Guard the C callback contract even for an incorrect adapter.
            for (auto* format = candidates; *format != AV_PIX_FMT_NONE; ++format) {
                if (*format == selected) return selected;
            }
            return AV_PIX_FMT_NONE;
        }
        SEMI_LOG_INFO("hardware format declined; selecting software output");
    }
    for (auto* format = candidates; *format != AV_PIX_FMT_NONE; ++format) {
        if (FfmpegVideoFrameBuffer::media_pixel_format(*format)) return *format;
    }
    return AV_PIX_FMT_NONE;
}

struct OutputObservation {
    int format = AV_PIX_FMT_NONE;
    int width = 0;
    int height = 0;
};

std::string ffmpeg_message(int error_code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(error_code, buffer.data(), buffer.size()) < 0) {
        return "FFmpeg error " + std::to_string(error_code);
    }
    return buffer.data();
}

VideoDecoderBackendError make_error(VideoDecoderBackendOperation operation, int error_code) {
    return VideoDecoderBackendError{
        .operation = operation,
        .native_code = error_code,
        .message = ffmpeg_message(error_code),
    };
}

VideoDecoderBackendError make_state_error(VideoDecoderBackendOperation operation,
                                          const char* message) {
    return VideoDecoderBackendError{
        .operation = operation,
        .native_code = AVERROR(EINVAL),
        .message = message,
    };
}

std::optional<std::int64_t> timestamp_us(const AVFrame& frame) noexcept {
    if (frame.best_effort_timestamp == AV_NOPTS_VALUE) {
        return std::nullopt;
    }
    return frame.best_effort_timestamp;
}

std::expected<void, VideoDecoderBackendError> prepare_packet(
    AVPacket& destination, const EncodedPacket& source, VideoDecoderBackendOperation operation) {
    if (source.payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(
            make_state_error(operation, "encoded packet payload is too large for FFmpeg"));
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

std::expected<std::unique_ptr<const contracts::media::VideoFrameBuffer>, VideoDecoderBackendError>
make_frame_buffer(AvFramePtr frame, FfmpegHardwareDecodeSession* hardware,
                  VideoDecoderBackendOperation operation) {
    const auto* description = frame
        ? av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format)) : nullptr;
    if (description && (description->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
        if (!hardware) {
            return std::unexpected(make_state_error(operation, "hardware frame without decode session"));
        }
        auto buffer = hardware->wrap_frame(std::move(frame));
        if (!buffer) {
            return std::unexpected(VideoDecoderBackendError{
                operation, buffer.error().native_code, std::move(buffer.error().message)});
        }
        if (!*buffer) {
            return std::unexpected(make_state_error(operation, "hardware adapter returned an empty frame"));
        }
        return std::move(*buffer);
    }
    if (frame == nullptr || frame->format < 0 ||
        !FfmpegVideoFrameBuffer::media_pixel_format(static_cast<AVPixelFormat>(frame->format))) {
        return std::unexpected(
            make_state_error(operation, "FFmpeg video frame has an unsupported pixel format"));
    }

    try {
        auto buffer = std::make_unique<FfmpegVideoFrameBuffer>(std::move(frame));
        const auto* cpu =
            contracts::media::resource_as<contracts::media::CpuVideoFrameResource>(*buffer);
        if (buffer->pixel_format() == contracts::media::VideoPixelFormat::Unknown ||
            buffer->width() == 0 || buffer->height() == 0 || !cpu || cpu->planes.empty()) {
            return std::unexpected(
                make_state_error(operation, "FFmpeg video frame has invalid planes"));
        }
        return std::unique_ptr<const contracts::media::VideoFrameBuffer>(std::move(buffer));
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(operation, AVERROR(ENOMEM)));
    }
}

std::expected<void, VideoDecoderBackendError>
append_received_frames(AVCodecContext& context,
                       AVFrame& frame,
                       DecodedVideoBatch& output,
                       FfmpegHardwareDecodeSession* hardware,
                       OutputObservation& observed,
                       VideoDecoderBackendOperation operation) {
    try {
        for (;;) {
            const int status = avcodec_receive_frame(&context, &frame);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return {};
            }
            if (status < 0) {
                return std::unexpected(make_error(operation, status));
            }

            if (frame.format != observed.format || frame.width != observed.width ||
                frame.height != observed.height) {
                const auto* description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame.format));
                SEMI_LOG_INFO("actual output: backend={} format={} size={}x{}",
                    description && (description->flags & AV_PIX_FMT_FLAG_HWACCEL) && hardware
                        ? hardware->name() : std::string_view("software"),
                    description ? description->name : "unknown", frame.width, frame.height);
                observed = {frame.format, frame.width, frame.height};
            }

            AvFramePtr owned_frame(av_frame_alloc());
            if (owned_frame == nullptr) {
                return std::unexpected(make_error(operation, AVERROR(ENOMEM)));
            }
            av_frame_move_ref(owned_frame.get(), &frame);
            const auto pts_us = timestamp_us(*owned_frame);
            auto buffer = make_frame_buffer(std::move(owned_frame), hardware, operation);
            if (!buffer) {
                return std::unexpected(std::move(buffer.error()));
            }

            std::unique_ptr<const contracts::media::VideoFrameBuffer> owner = std::move(*buffer);
            output.push_back(DecodedVideo{
                .buffer = std::move(owner),
                .pts_us = pts_us,
            });
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(operation, AVERROR(ENOMEM)));
    }
}

std::expected<DecodedVideoBatch, VideoDecoderBackendError>
send_packet_and_collect_frames(AVCodecContext& context,
                               AVFrame& frame,
                               AVPacket* packet,
                               FfmpegHardwareDecodeSession* hardware,
                               OutputObservation& observed,
                               VideoDecoderBackendOperation operation) {
    try {
        DecodedVideoBatch output;
        for (;;) {
            const int status = avcodec_send_packet(&context, packet);
            if (status == AVERROR(EAGAIN)) {
                auto received = append_received_frames(
                    context, frame, output, hardware, observed, operation);
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

        auto received = append_received_frames(context, frame, output, hardware, observed, operation);
        if (!received) {
            return std::unexpected(std::move(received.error()));
        }
        return output;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(operation, AVERROR(ENOMEM)));
    }
}

} // namespace

struct FfmpegVideoDecoderBackend::Impl {
    std::shared_ptr<hardware_decode::FfmpegHardwareDecodeProvider> hardware;
    // Context callbacks borrow session; destroy/join the context first.
    std::unique_ptr<FfmpegHardwareDecodeSession> session;
    AvCodecContextPtr context;
    AvPacketPtr packet;
    AvFramePtr frame;
    bool draining = false;
    OutputObservation observed;
};

FfmpegVideoDecoderBackend::FfmpegVideoDecoderBackend(
    std::shared_ptr<hardware_decode::FfmpegHardwareDecodeProvider> hardware)
    : impl_(std::make_unique<Impl>()) {
    impl_->hardware = std::move(hardware);
}

FfmpegVideoDecoderBackend::~FfmpegVideoDecoderBackend() {
    unconfigure();
}

std::expected<void, VideoDecoderBackendError>
FfmpegVideoDecoderBackend::configure(const contracts::media::VideoCodecConfig& config) {
    if (impl_->context != nullptr) {
        return std::unexpected(
            make_state_error(VideoDecoderBackendOperation::Configure,
                             "FFmpeg video decoder backend is already configured"));
    }
    if (config.common.codec_name.empty() ||
        config.coded_width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        config.coded_height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(make_state_error(VideoDecoderBackendOperation::Configure,
                                                "video decoder configuration is incomplete"));
    }

    const AVCodec* codec = avcodec_find_decoder_by_name(config.common.codec_name.c_str());
    if (codec == nullptr || codec->type != AVMEDIA_TYPE_VIDEO) {
        return std::unexpected(
            make_state_error(VideoDecoderBackendOperation::Configure,
                             "FFmpeg could not find the requested video decoder"));
    }

    AvPacketPtr packet(av_packet_alloc());
    AvFramePtr frame(av_frame_alloc());
    if (packet == nullptr || frame == nullptr) {
        return std::unexpected(
            make_error(VideoDecoderBackendOperation::Configure, AVERROR(ENOMEM)));
    }

    // Every attempt starts with fresh extradata/context: a failed hardware open
    // must not leak callbacks, surfaces or mutated parameters into software.
    auto open_context = [&](FfmpegHardwareDecodeSession* session)
        -> std::expected<AvCodecContextPtr, VideoDecoderBackendError> {
        AvCodecContextPtr context(avcodec_alloc_context3(codec));
        if (!context) {
            return std::unexpected(
                make_error(VideoDecoderBackendOperation::Configure, AVERROR(ENOMEM)));
        }
        context->get_format = select_format;
        if (config.coded_width != 0) {
            context->width = static_cast<int>(config.coded_width);
        }
        if (config.coded_height != 0) {
            context->height = static_cast<int>(config.coded_height);
        }
        if (config.profile) {
            context->profile = *config.profile;
        }
        if (config.level) {
            context->level = *config.level;
        }
        context->pkt_timebase = AV_TIME_BASE_Q;

        if (!config.common.extradata.empty()) {
            if (config.common.extradata.size() >
                static_cast<std::size_t>(std::numeric_limits<int>::max() -
                                         AV_INPUT_BUFFER_PADDING_SIZE)) {
                return std::unexpected(
                    make_state_error(VideoDecoderBackendOperation::Configure,
                                     "video decoder extradata is too large for FFmpeg"));
            }
            context->extradata_size = static_cast<int>(config.common.extradata.size());
            context->extradata = static_cast<std::uint8_t*>(
                av_mallocz(config.common.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (context->extradata == nullptr) {
                return std::unexpected(
                    make_error(VideoDecoderBackendOperation::Configure, AVERROR(ENOMEM)));
            }
            std::memcpy(context->extradata, config.common.extradata.data(),
                        config.common.extradata.size());
        }

        if (session) {
            context->opaque = session;
            auto configured = session->configure(*context);
            if (!configured) {
                return std::unexpected(VideoDecoderBackendError{
                    VideoDecoderBackendOperation::Configure, configured.error().native_code,
                    std::move(configured.error().message)});
            }
        }
        const int status = avcodec_open2(context.get(), codec, nullptr);
        if (status < 0) {
            return std::unexpected(make_error(VideoDecoderBackendOperation::Configure, status));
        }

        return context;
    };

    std::unique_ptr<FfmpegHardwareDecodeSession> session;
    if (impl_->hardware) {
        auto prepared = impl_->hardware->prepare(*codec);
        if (prepared) {
            session = std::move(*prepared);
            if (session) SEMI_LOG_INFO("attempting hardware decode: {}", session->name());
        } else {
            SEMI_LOG_INFO("hardware unavailable; using software: code={} {}",
                          prepared.error().native_code, prepared.error().message);
        }
    }
    auto context = open_context(session.get());
    if (!context && session) {
        SEMI_LOG_WARN("hardware setup/open failed; retrying software: code={} {}",
                      context.error().native_code, context.error().message);
        session.reset();
        context = open_context(nullptr);
    }
    if (!context) return std::unexpected(std::move(context.error()));

    impl_->session = std::move(session);
    impl_->context = std::move(*context);
    impl_->packet = std::move(packet);
    impl_->frame = std::move(frame);
    impl_->draining = false;
    impl_->observed = {};
    return {};
}

std::expected<DecodedVideoBatch, VideoDecoderBackendError>
FfmpegVideoDecoderBackend::decode(const EncodedPacket& packet) {
    if (impl_->context == nullptr) {
        return std::unexpected(make_state_error(VideoDecoderBackendOperation::Decode,
                                                "FFmpeg video decoder backend is not configured"));
    }
    if (impl_->draining) {
        return std::unexpected(make_state_error(VideoDecoderBackendOperation::Decode,
                                                "FFmpeg video decoder must be reset after drain"));
    }

    auto prepared = prepare_packet(*impl_->packet, packet, VideoDecoderBackendOperation::Decode);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    return send_packet_and_collect_frames(*impl_->context, *impl_->frame, impl_->packet.get(),
                                          impl_->session.get(), impl_->observed,
                                          VideoDecoderBackendOperation::Decode);
}

std::expected<DecodedVideoBatch, VideoDecoderBackendError> FfmpegVideoDecoderBackend::drain() {
    if (impl_->context == nullptr) {
        return std::unexpected(make_state_error(VideoDecoderBackendOperation::Drain,
                                                "FFmpeg video decoder backend is not configured"));
    }
    if (impl_->draining) {
        return DecodedVideoBatch{};
    }

    auto output = send_packet_and_collect_frames(*impl_->context, *impl_->frame, nullptr,
                                                 impl_->session.get(), impl_->observed,
                                                 VideoDecoderBackendOperation::Drain);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    impl_->draining = true;
    return output;
}

void FfmpegVideoDecoderBackend::reset() noexcept {
    if (impl_->context != nullptr) {
        avcodec_flush_buffers(impl_->context.get());
        impl_->draining = false;
    }
}

void FfmpegVideoDecoderBackend::unconfigure() noexcept {
    if (!impl_) {
        return;
    }
    impl_->frame.reset();
    impl_->packet.reset();
    impl_->context.reset();
    impl_->session.reset();
    impl_->draining = false;
    impl_->observed = {};
}

} // namespace semi::infra::ffmpeg::video_decoder
