#include "ioc/ioc_container.hpp"

#include "application/api_layer.hpp"
#include "domain/resource/audio_frame_store/audio_frame_store.hpp"
#include "domain/resource/audio_packet_queue/audio_packet_queue.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_decoder/default_audio_decoder.hpp"
#include "domain/worker/audio_output/default_audio_output.hpp"
#include "domain/worker/audio_resampler/default_audio_resampler.hpp"
#include "domain/worker/demuxer/default_demuxer.hpp"
#include "infrastructure/audio_output/null_audio_output_backend.hpp"
#include "infrastructure/ffmpeg/audio_decoder/ffmpeg_audio_decoder_backend.hpp"
#include "infrastructure/ffmpeg/audio_resampler/ffmpeg_audio_resampler_backend.hpp"
#include "infrastructure/ffmpeg/demuxer/ffmpeg_demuxer_backend.hpp"
#include "infrastructure/log/log.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#define SEMI_LOG_TAG "ioc"

namespace semi::ioc {

IoCContainer& IoCContainer::instance() {
    static IoCContainer container;
    return container;
}

bool IoCContainer::assemble() noexcept {
    if (assembled_) {
        SEMI_LOG_INFO("assemble skipped: already assembled");
        return true;
    }

    SEMI_LOG_INFO("assemble begin");
    try {
        auto notifier = std::make_shared<infra::DefaultNotifier>();
        auto generation = std::make_shared<domain::Generation>();
        auto audio_packet_queue = std::make_shared<domain::AudioPacketQueue>(notifier);
        auto decoded_audio_frame_store = std::make_shared<domain::AudioFrameStore>(notifier);
        auto playback_audio_frame_store = std::make_shared<domain::AudioFrameStore>(notifier);

        auto demuxer_backend =
            std::make_shared<infra::ffmpeg::demuxer::FfmpegDemuxerBackend>();
        auto audio_decoder_backend =
            std::make_shared<infra::ffmpeg::audio_decoder::FfmpegAudioDecoderBackend>();
        auto audio_resampler_backend =
            std::make_shared<infra::ffmpeg::audio_resampler::FfmpegAudioResamplerBackend>();
        auto audio_output_backend =
            std::make_shared<infra::audio_output::NullAudioOutputBackend>();

        auto demuxer = std::make_shared<domain::DefaultDemuxer>(
            demuxer_backend, audio_packet_queue, notifier, generation);
        auto audio_decoder = std::make_shared<domain::DefaultAudioDecoder>(
            audio_packet_queue, decoded_audio_frame_store, audio_decoder_backend, notifier, generation);
        auto audio_resampler = std::make_shared<domain::DefaultAudioResampler>(
            decoded_audio_frame_store, playback_audio_frame_store, audio_resampler_backend, notifier,
            generation);
        auto audio_output = std::make_shared<domain::DefaultAudioOutput>(
            playback_audio_frame_store, audio_output_backend, notifier, generation);

        auto api_layer = std::make_shared<application::ApiLayer>(
            demuxer, audio_decoder, audio_resampler, audio_output);
        if (!api_layer->start()) {
            SEMI_LOG_ERROR("ApiLayer start failed");
            return false;
        }
        notifier_ = std::move(notifier);
        generation_ = std::move(generation);
        audio_packet_queue_ = std::move(audio_packet_queue);
        decoded_audio_frame_store_ = std::move(decoded_audio_frame_store);
        playback_audio_frame_store_ = std::move(playback_audio_frame_store);
        demuxer_ = std::move(demuxer);
        audio_decoder_ = std::move(audio_decoder);
        audio_resampler_ = std::move(audio_resampler);
        audio_output_ = std::move(audio_output);
        api_layer_ = std::move(api_layer);
    } catch (...) {
        SEMI_LOG_ERROR("ApiLayer assemble failed");
        return false;
    }
    assembled_ = true;
    SEMI_LOG_INFO("assemble done");
    return true;
}

bool IoCContainer::dispose() noexcept {
    if (!assembled_) {
        SEMI_LOG_INFO("dispose skipped: not assembled");
        return true;
    }

    SEMI_LOG_INFO("dispose begin");
    // 逆序：依赖者先于被依赖者。ApiLayer 必须先排空其命令线程。
    if (api_layer_ && !api_layer_->stop()) {
        SEMI_LOG_ERROR("ApiLayer stop failed");
        return false;
    }
    api_layer_.reset();
    audio_output_.reset();
    audio_resampler_.reset();
    audio_decoder_.reset();
    demuxer_.reset();
    playback_audio_frame_store_.reset();
    decoded_audio_frame_store_.reset();
    audio_packet_queue_.reset();
    generation_.reset();
    notifier_.reset();
    assembled_ = false;
    SEMI_LOG_INFO("dispose done");
    return true;
}

bool IoCContainer::is_assembled() const noexcept {
    return assembled_;
}

std::shared_ptr<application::ApiLayer> IoCContainer::api_layer() const noexcept {
    return api_layer_;
}

} // namespace semi::ioc
