#pragma once

#include "contracts/audio_output/audio_output_backend.hpp"
#include "domain/resource/audio_frame_store/audio_frame_source.hpp"
#include "domain/resource/audio_frame_store/audio_frame_store_events.hpp"
#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_output/audio_output.hpp"
#include "domain/worker/audio_output/audio_playback_clock.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <deque>

namespace semi::domain {

class DefaultAudioOutput final : public AudioOutput {
public:
    DefaultAudioOutput(std::shared_ptr<AudioFrameSource> audio_frame_source,
                       std::shared_ptr<AudioOutputBackend> backend,
                       std::shared_ptr<infra::Notifier> notifier,
                       std::shared_ptr<contracts::audio_output::AudioOutputRealTimeNotifier>
                           realtime_notifier,
                       std::shared_ptr<Generation> generation);
    ~DefaultAudioOutput() override;

    DefaultAudioOutput(const DefaultAudioOutput&) = delete;
    DefaultAudioOutput& operator=(const DefaultAudioOutput&) = delete;
    DefaultAudioOutput(DefaultAudioOutput&&) = delete;
    DefaultAudioOutput& operator=(DefaultAudioOutput&&) = delete;

    [[nodiscard]] std::expected<AudioOutputConfigureResult, AudioOutputError>
    configure(const AudioOutputOptions& options) override;

    [[nodiscard]] std::expected<void, AudioOutputError> start_playback() override;

    [[nodiscard]] std::expected<void, AudioOutputError> pause_playback() override;

    void unconfigure() noexcept override;

private:
    class ProgressSink final : public infra::RealTimeNotificationSink<std::uint32_t> {
    public:
        explicit ProgressSink(DefaultAudioOutput& owner) noexcept : owner_(owner) {}

        void on_realtime_notification(const std::uint32_t& confirmed_frames) noexcept override;

    private:
        DefaultAudioOutput& owner_;
    };

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

    enum class PlaybackPhase : std::uint8_t {
        Running,
        Draining,
        Finished,
    };

    struct ConfigureCommand {
        AudioOutputOptions options;
        std::promise<std::expected<AudioOutputConfigureResult, AudioOutputError>> completion;
    };

    struct UnconfigureCommand {
        std::promise<void> completion;
    };

    struct StartPlaybackCommand {
        std::promise<std::expected<void, AudioOutputError>> completion;
    };

    struct PausePlaybackCommand {
        std::promise<std::expected<void, AudioOutputError>> completion;
    };

    using ControlCommand =
        std::variant<ConfigureCommand, UnconfigureCommand, StartPlaybackCommand, PausePlaybackCommand>;

    enum class DataStepResult : std::uint8_t {
        Handled,
        NoPendingFrame,
    };

    void worker_main() noexcept;
    void process_command(ConfigureCommand& command) noexcept;
    void process_command(UnconfigureCommand& command) noexcept;
    void process_command(StartPlaybackCommand& command) noexcept;
    void process_command(PausePlaybackCommand& command) noexcept;
    void shutdown_worker() noexcept;

    [[nodiscard]] bool should_process_data_locked() const noexcept;
    void handle_generation_change_if_needed() noexcept;
    [[nodiscard]] bool reset_backend_for_generation(Generation::Value generation) noexcept;
    [[nodiscard]] DataStepResult try_submit_pending_frame() noexcept;
    [[nodiscard]] DataStepResult try_drain_backend() noexcept;
    void read_next_input_to_pending() noexcept;
    void handle_input_item(AudioFrameStoreItem item) noexcept;
    void handle_audio_frame(AudioFrame frame, Generation::Value current_generation) noexcept;
    void handle_end_of_input(Generation::Value generation) noexcept;
    void handle_backend_failure(
        AudioOutputBackendError error,
        std::optional<Generation::Value> generation_override = std::nullopt) noexcept;
    void notify_backend_failure(AudioOutputBackendError error, Generation::Value generation) noexcept;
    void notify_playback_finished(Generation::Value generation) noexcept;
    void on_audio_frames_consumed(std::uint32_t confirmed_frames) noexcept;

    [[nodiscard]] bool transition_worker_locked(WorkerEvent event) noexcept;
    [[nodiscard]] bool transition_session_locked(SessionEvent event) noexcept;

    std::shared_ptr<AudioFrameSource> audio_frame_source_;
    std::shared_ptr<AudioOutputBackend> backend_;
    std::shared_ptr<infra::Notifier> notifier_;
    std::shared_ptr<contracts::audio_output::AudioOutputRealTimeNotifier> realtime_notifier_;
    std::shared_ptr<Generation> generation_;
    ProgressSink progress_sink_;

    std::shared_ptr<infra::Notifier::Subscription> audio_frame_store_not_empty_subscription_;
    std::shared_ptr<infra::Notifier::Subscription> generation_changed_subscription_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    WorkerState worker_state_ = WorkerState::Starting;
    SessionState session_state_ = SessionState::Constructed;
    PlaybackPhase phase_ = PlaybackPhase::Running;
    std::deque<ControlCommand> commands_;

    std::optional<AudioFrame> pending_frame_;
    std::atomic<Generation::Value> active_generation_{0};
    std::uint32_t playback_sample_rate_ = 0;
    bool playback_enabled_ = false;
    bool discarding_stale_generation_ = false;
    bool input_not_empty_hint_ = false;
    std::atomic_bool backend_progress_hint_ = false;
    AudioPlaybackClockState playback_clock_;
};

} // namespace semi::domain
