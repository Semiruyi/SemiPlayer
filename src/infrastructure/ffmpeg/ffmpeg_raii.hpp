#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <memory>

namespace semi::infra::ffmpeg {

struct AvCodecContextDeleter {
    void operator()(AVCodecContext* context) const noexcept {
        avcodec_free_context(&context);
    }
};

struct AvFrameDeleter {
    void operator()(AVFrame* frame) const noexcept {
        av_frame_free(&frame);
    }
};

struct AvPacketDeleter {
    void operator()(AVPacket* packet) const noexcept {
        av_packet_free(&packet);
    }
};

struct AvFormatInputContextDeleter {
    void operator()(AVFormatContext* context) const noexcept {
        avformat_close_input(&context);
    }
};

using AvCodecContextPtr = std::unique_ptr<AVCodecContext, AvCodecContextDeleter>;
using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;
using AvPacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;
using AvFormatInputContextPtr = std::unique_ptr<AVFormatContext, AvFormatInputContextDeleter>;

} // namespace semi::infra::ffmpeg
