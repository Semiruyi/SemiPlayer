#include "infrastructure/ffmpeg/video_decoder/ffmpeg_video_decoder_backend.hpp"

#include "infrastructure/ffmpeg/ffmpeg_raii.hpp"
#include "infrastructure/ffmpeg/video_decoder/ffmpeg_video_frame_buffer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace semi::infra::ffmpeg::video_decoder {
namespace {

using contracts::demuxer::packet::EncodedPacket;
using contracts::media::DecodedVideo;
using contracts::video_decoder::DecodedVideoBatch;
using contracts::video_decoder::VideoDecoderBackendError;
using contracts::video_decoder::VideoDecoderBackendOperation;

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

std::expected<void, VideoDecoderBackendError>
prepare_packet(AVPacket& destination, const EncodedPacket& source,
               VideoDecoderBackendOperation operation) {
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

std::expected<std::unique_ptr<const FfmpegVideoFrameBuffer>, VideoDecoderBackendError>
make_frame_buffer(AvFramePtr frame, VideoDecoderBackendOperation operation) {
    if (frame == nullptr || frame->format < 0 ||
        !FfmpegVideoFrameBuffer::media_pixel_format(static_cast<AVPixelFormat>(frame->format))) {
        return std::unexpected(make_state_error(operation,
                                                "FFmpeg video frame has an unsupported pixel format"));
    }

    try {
        auto buffer = std::make_unique<FfmpegVideoFrameBuffer>(std::move(frame));
        if (buffer->pixel_format() == contracts::media::VideoPixelFormat::Unknown ||
            buffer->width() == 0 || buffer->height() == 0 || buffer->plane_count() == 0) {
            return std::unexpected(make_state_error(operation, "FFmpeg video frame has invalid planes"));
        }
        return std::unique_ptr<const FfmpegVideoFrameBuffer>(std::move(buffer));
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(operation, AVERROR(ENOMEM)));
    }
}

std::expected<void, VideoDecoderBackendError>
append_received_frames(AVCodecContext& context, AVFrame& frame, DecodedVideoBatch& output,
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

            AvFramePtr owned_frame(av_frame_alloc());
            if (owned_frame == nullptr) {
                return std::unexpected(make_error(operation, AVERROR(ENOMEM)));
            }
            av_frame_move_ref(owned_frame.get(), &frame);
            const auto pts_us = timestamp_us(*owned_frame);
            auto buffer = make_frame_buffer(std::move(owned_frame), operation);
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
send_packet_and_collect_frames(AVCodecContext& context, AVFrame& frame, AVPacket* packet,
                               VideoDecoderBackendOperation operation) {
    try {
        DecodedVideoBatch output;
        for (;;) {
            const int status = avcodec_send_packet(&context, packet);
            if (status == AVERROR(EAGAIN)) {
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

        auto received = append_received_frames(context, frame, output, operation);
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
    AvCodecContextPtr context;
    AvPacketPtr packet;
    AvFramePtr frame;
    bool draining = false;
};

FfmpegVideoDecoderBackend::FfmpegVideoDecoderBackend() : impl_(std::make_unique<Impl>()) {}

FfmpegVideoDecoderBackend::~FfmpegVideoDecoderBackend() {
    unconfigure();
}

std::expected<void, VideoDecoderBackendError>
FfmpegVideoDecoderBackend::configure(const contracts::media::VideoCodecConfig& config) {
    if (impl_->context != nullptr) {
        return std::unexpected(make_state_error(VideoDecoderBackendOperation::Configure,
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
        return std::unexpected(make_state_error(VideoDecoderBackendOperation::Configure,
                                                "FFmpeg could not find the requested video decoder"));
    }

    AvCodecContextPtr context(avcodec_alloc_context3(codec));
    AvPacketPtr packet(av_packet_alloc());
    AvFramePtr frame(av_frame_alloc());
    if (context == nullptr || packet == nullptr || frame == nullptr) {
        return std::unexpected(make_error(VideoDecoderBackendOperation::Configure, AVERROR(ENOMEM)));
    }

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
            static_cast<std::size_t>(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE)) {
            return std::unexpected(make_state_error(VideoDecoderBackendOperation::Configure,
                                                    "video decoder extradata is too large for FFmpeg"));
        }
        context->extradata_size = static_cast<int>(config.common.extradata.size());
        context->extradata = static_cast<std::uint8_t*>(
            av_mallocz(config.common.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (context->extradata == nullptr) {
            return std::unexpected(make_error(VideoDecoderBackendOperation::Configure, AVERROR(ENOMEM)));
        }
        std::memcpy(context->extradata, config.common.extradata.data(), config.common.extradata.size());
    }

    const int status = avcodec_open2(context.get(), codec, nullptr);
    if (status < 0) {
        return std::unexpected(make_error(VideoDecoderBackendOperation::Configure, status));
    }

    impl_->context = std::move(context);
    impl_->packet = std::move(packet);
    impl_->frame = std::move(frame);
    impl_->draining = false;
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
                                          VideoDecoderBackendOperation::Decode);
}

std::expected<DecodedVideoBatch, VideoDecoderBackendError>
FfmpegVideoDecoderBackend::drain() {
    if (impl_->context == nullptr) {
        return std::unexpected(make_state_error(VideoDecoderBackendOperation::Drain,
                                                "FFmpeg video decoder backend is not configured"));
    }
    if (impl_->draining) {
        return DecodedVideoBatch{};
    }

    auto output = send_packet_and_collect_frames(*impl_->context, *impl_->frame, nullptr,
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
    impl_->draining = false;
}

} // namespace semi::infra::ffmpeg::video_decoder
