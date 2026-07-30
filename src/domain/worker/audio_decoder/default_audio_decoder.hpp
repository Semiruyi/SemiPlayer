#pragma once

#include "contracts/audio_decoder/audio_decoder_backend.hpp"
#include "domain/resource/audio_frame_store/audio_frame_sink.hpp"
#include "domain/resource/audio_frame_store/audio_frame_store_events.hpp"
#include "domain/resource/audio_packet_queue/audio_packet_queue_events.hpp"
#include "domain/resource/audio_packet_queue/audio_packet_source.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_decoder/audio_decoder.hpp"
#include "domain/worker/audio_decoder/audio_decoder_events.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace semi::domain {

class DefaultAudioDecoder final : public AudioDecoder {
public:
    DefaultAudioDecoder(std::shared_ptr<AudioPacketSource> audio_packet_source,
                        std::shared_ptr<AudioFrameSink> audio_frame_sink,
                        std::shared_ptr<AudioDecoderBackend> backend,
                        std::shared_ptr<infra::Notifier> notifier,
                        std::shared_ptr<Generation> generation);
    ~DefaultAudioDecoder() override;

    DefaultAudioDecoder(const DefaultAudioDecoder&) = delete;
    DefaultAudioDecoder& operator=(const DefaultAudioDecoder&) = delete;
    DefaultAudioDecoder(DefaultAudioDecoder&&) = delete;
    DefaultAudioDecoder& operator=(DefaultAudioDecoder&&) = delete;

    [[nodiscard]] std::expected<void, AudioDecoderError>
    configure(const contracts::media::AudioCodecConfig& config) override;

    [[nodiscard]] std::expected<void, AudioDecoderError> start() override;

    void stop() noexcept override;
    void unconfigure() noexcept override;

private:
    enum class State : std::uint8_t {
        Constructed,
        Configured,
        Running,
        Stopping,
        Failed,
    };

    enum class Event : std::uint8_t {
        ConfigureSucceeded,
        StartRequested,
        WorkerStartFailed,
        StopRequested,
        WorkerStopped,
        BackendFailed,
        UnconfigureRequested,
    };

    enum class WorkAction : std::uint8_t {
        Stop,
        RetryPendingOutputs,
        ReadInput,
    };

    enum class WorkerExit : std::uint8_t {
        Stopped,
        Failed,
    };

    void worker_main() noexcept;
    [[nodiscard]] WorkAction wait_for_work(bool has_pending_outputs);
    [[nodiscard]] std::optional<WorkerExit> read_and_process_input(bool& input_was_read);
    [[nodiscard]] std::optional<WorkerExit> process_input_item(AudioPacketQueueItem&& item);
    [[nodiscard]] std::optional<WorkerExit> process_audio_packet(AudioPacket&& packet);
    [[nodiscard]] std::optional<WorkerExit>
    process_end_of_input(AudioPacketEndOfInput end_of_input);

    void append_decoded_audio(contracts::audio_decoder::DecodedAudioBatch&& decoded,
                              Generation::Value generation);
    [[nodiscard]] bool flush_pending_outputs();
    void synchronize_generation() noexcept;

    [[nodiscard]] bool transition_locked(Event event) noexcept;
    void complete_worker_locked(WorkerExit exit) noexcept;
    void notify_backend_failure(AudioDecoderBackendError error) noexcept;

    std::shared_ptr<AudioPacketSource> audio_packet_source_;
    std::shared_ptr<AudioFrameSink> audio_frame_sink_;
    std::shared_ptr<AudioDecoderBackend> backend_;
    std::shared_ptr<infra::Notifier> notifier_;
    std::shared_ptr<Generation> generation_;

    std::shared_ptr<infra::Notifier::Subscription> audio_queue_not_empty_subscription_;
    std::shared_ptr<infra::Notifier::Subscription> audio_frame_store_not_full_subscription_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    State state_ = State::Constructed;
    bool worker_running_ = false;

    std::atomic_bool input_not_empty_hint_{false};
    std::atomic_bool output_not_full_hint_{false};

    std::deque<AudioFrameStoreItem> pending_outputs_;
    Generation::Value active_generation_ = 0;
    bool input_exhausted_ = false;
};

} // namespace semi::domain
