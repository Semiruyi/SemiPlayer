#pragma once

#include "contracts/audio_resampler/audio_resampler_backend.hpp"
#include "domain/resource/audio_frame_store/audio_frame_sink.hpp"
#include "domain/resource/audio_frame_store/audio_frame_source.hpp"
#include "domain/resource/audio_frame_store/audio_frame_store_events.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_resampler/audio_resampler.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>

namespace semi::domain {

class DefaultAudioResampler final : public AudioResampler {
public:
    DefaultAudioResampler(std::shared_ptr<AudioFrameSource> audio_frame_source,
                          std::shared_ptr<AudioFrameSink> audio_frame_sink,
                          std::shared_ptr<AudioResamplerBackend> backend,
                          std::shared_ptr<infra::Notifier> notifier,
                          std::shared_ptr<Generation> generation);
    ~DefaultAudioResampler() override;

    DefaultAudioResampler(const DefaultAudioResampler&) = delete;
    DefaultAudioResampler& operator=(const DefaultAudioResampler&) = delete;
    DefaultAudioResampler(DefaultAudioResampler&&) = delete;
    DefaultAudioResampler& operator=(DefaultAudioResampler&&) = delete;

    [[nodiscard]] std::expected<void, AudioResamplerError>
    configure(const contracts::media::AudioPcmFormat& input_format,
              const contracts::media::AudioPcmFormat& output_format) override;

    void unconfigure() noexcept override;

private:
    enum class WorkerState : std::uint8_t {
        Starting,
        Alive,
        ShuttingDown,
        Stopped,
    };
    enum class WorkerEvent : std::uint8_t {
        Started,
        ShutdownRequested,
        Stopped,
    };

    enum class SessionState : std::uint8_t {
        Constructed,
        Configuring,
        Configured,
        Unconfiguring,
        Failed,
    };
    enum class SessionEvent : std::uint8_t {
        ConfigureRequested,
        ConfigureSucceeded,
        ConfigureFailed,
        UnconfigureRequested,
        UnconfigureSucceeded,
        BackendFailed,
    };

    struct ConfigureCommand {
        contracts::media::AudioPcmFormat input_format;
        contracts::media::AudioPcmFormat output_format;
        std::promise<std::expected<void, AudioResamplerError>> completion;
    };

    struct UnconfigureCommand {
        std::promise<void> completion;
    };

    using ControlCommand = std::variant<ConfigureCommand, UnconfigureCommand>;

    enum class PendingOutputPushResult : std::uint8_t {
        NoPending,
        Handled,
    };

    void worker_main() noexcept;
    void process_command(ConfigureCommand& command) noexcept;
    void process_command(UnconfigureCommand& command) noexcept;
    void shutdown_worker() noexcept;

    [[nodiscard]] bool should_process_data_locked() const noexcept;
    void adopt_generation_if_needed(Generation::Value current_generation) noexcept;
    [[nodiscard]] PendingOutputPushResult try_push_pending_output() noexcept;
    void read_next_input_to_pending() noexcept;
    void handle_input_item(AudioFrameStoreItem item) noexcept;
    void handle_audio_frame(AudioFrame frame, Generation::Value current_generation) noexcept;
    void handle_end_of_input(Generation::Value generation) noexcept;
    void store_resampled_outputs(contracts::audio_resampler::ResampledAudioBatch resampled,
                                 Generation::Value generation,
                                 bool append_end_of_input) noexcept;
    void handle_backend_failure(AudioResamplerBackendError error) noexcept;
    void notify_backend_failure(AudioResamplerBackendError error) noexcept;

    [[nodiscard]] bool transition_worker_locked(WorkerEvent event) noexcept;
    [[nodiscard]] bool transition_session_locked(SessionEvent event) noexcept;

    std::shared_ptr<AudioFrameSource> audio_frame_source_;
    std::shared_ptr<AudioFrameSink> audio_frame_sink_;
    std::shared_ptr<AudioResamplerBackend> backend_;
    std::shared_ptr<infra::Notifier> notifier_;
    std::shared_ptr<Generation> generation_;

    std::shared_ptr<infra::Notifier::Subscription> audio_frame_store_not_empty_subscription_;
    std::shared_ptr<infra::Notifier::Subscription> audio_frame_store_not_full_subscription_;
    std::shared_ptr<infra::Notifier::Subscription> generation_changed_subscription_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    WorkerState worker_state_ = WorkerState::Starting;
    SessionState session_state_ = SessionState::Constructed;
    std::deque<ControlCommand> commands_;

    std::deque<AudioFrameStoreItem> pending_outputs_;
    Generation::Value active_generation_ = 0;
    bool input_exhausted_ = false;
    bool input_not_empty_hint_ = false;
    bool output_not_full_hint_ = false;
};

} // namespace semi::domain
