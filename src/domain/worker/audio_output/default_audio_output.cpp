#include "domain/worker/audio_output/default_audio_output.hpp"

#include "domain/resource/generation/generation_events.hpp"
#include "domain/worker/audio_output/audio_output_events.hpp"

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace semi::domain {
namespace {

AudioOutputError invalid_state(std::string message) {
    return AudioOutputError{
        .code = AudioOutputErrorCode::InvalidState,
        .message = std::move(message),
        .backend_error = std::nullopt,
    };
}

AudioOutputBackendError backend_exception(AudioOutputBackendOperation operation,
                                          std::string message) {
    return AudioOutputBackendError{
        .operation = operation,
        .native_code = 0,
        .message = std::move(message),
    };
}

AudioOutputError backend_failure(AudioOutputBackendError error) {
    return AudioOutputError{
        .code = AudioOutputErrorCode::BackendFailure,
        .message = error.message,
        .backend_error = std::move(error),
    };
}

} // namespace

void DefaultAudioOutput::ProgressSink::on_realtime_notification(
    const std::uint32_t& confirmed_frames) noexcept {
    owner_.on_audio_frames_consumed(confirmed_frames);
}

DefaultAudioOutput::DefaultAudioOutput(std::shared_ptr<AudioFrameSource> audio_frame_source,
                                       std::shared_ptr<AudioOutputBackend> backend,
                                       std::shared_ptr<infra::Notifier> notifier,
                                       std::shared_ptr<contracts::audio_output::AudioOutputRealTimeNotifier>
                                           realtime_notifier,
                                       std::shared_ptr<Generation> generation)
    : audio_frame_source_(std::move(audio_frame_source)),
      backend_(std::move(backend)),
      notifier_(std::move(notifier)),
      realtime_notifier_(std::move(realtime_notifier)),
      generation_(std::move(generation)),
      progress_sink_(*this),
      worker_([this] {
          worker_main();
      }) {
    if (realtime_notifier_) {
        (void)realtime_notifier_->register_sink(progress_sink_);
    }
    if (!notifier_) {
        return;
    }

    audio_frame_store_not_empty_subscription_ = notifier_->subscribe<AudioFrameStoreNotEmpty>(
        [this](const AudioFrameStoreNotEmpty&) {
            {
                std::lock_guard lock(mutex_);
                input_not_empty_hint_ = true;
            }
            cv_.notify_one();
        });
    generation_changed_subscription_ = notifier_->subscribe<GenerationChanged>(
        [this](const GenerationChanged&) {
            cv_.notify_one();
        });
}

DefaultAudioOutput::~DefaultAudioOutput() {
    shutdown_worker();
    audio_frame_store_not_empty_subscription_.reset();
    generation_changed_subscription_.reset();
    if (backend_) {
        backend_->unconfigure();
    }
    if (realtime_notifier_) {
        if (realtime_notifier_->sealed()) {
            (void)realtime_notifier_->unseal();
        }
        (void)realtime_notifier_->unregister_sink(progress_sink_);
    }
}

std::expected<AudioOutputConfigureResult, AudioOutputError>
DefaultAudioOutput::configure(const AudioOutputOptions& options) {
    ConfigureCommand command;
    command.options = options;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

void DefaultAudioOutput::unconfigure() noexcept {
    UnconfigureCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    completion.wait();
}

std::expected<void, AudioOutputError> DefaultAudioOutput::start_playback() {
    StartPlaybackCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

std::expected<void, AudioOutputError> DefaultAudioOutput::pause_playback() {
    PausePlaybackCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

void DefaultAudioOutput::on_audio_frames_consumed(
    std::uint32_t confirmed_frames) noexcept {
    playback_clock_.on_audio_frames_consumed(confirmed_frames);
    backend_progress_hint_.store(true, std::memory_order_release);
    cv_.notify_one();
}

void DefaultAudioOutput::worker_main() noexcept {
    std::unique_lock lock(mutex_);
    if (worker_state_ == WorkerState::Starting) {
        const bool started = transition_worker_locked(WorkerEvent::Started);
        assert(started);
    }
    cv_.notify_all();
    for (;;) {
        cv_.wait(lock, [this] {
            return worker_state_ == WorkerState::ShuttingDown || !commands_.empty() ||
                   should_process_data_locked();
        });
        if (worker_state_ == WorkerState::ShuttingDown) {
            break;
        }
        if (!commands_.empty()) {
            ControlCommand command = std::move(commands_.front());
            commands_.pop_front();
            lock.unlock();
            std::visit([this](auto& value) { process_command(value); }, command);
            lock.lock();
            continue;
        }
        if (should_process_data_locked()) {
            lock.unlock();
            handle_generation_change_if_needed();
            if (try_submit_pending_frame() == DataStepResult::NoPendingFrame) {
                if (try_drain_backend() == DataStepResult::NoPendingFrame) {
                    read_next_input_to_pending();
                }
            }
            lock.lock();
        }
    }
    const bool stopped = transition_worker_locked(WorkerEvent::Stopped);
    assert(stopped);
    cv_.notify_all();
}

void DefaultAudioOutput::shutdown_worker() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (worker_state_ == WorkerState::Stopped) {
            return;
        }
        const bool requested = transition_worker_locked(WorkerEvent::ShutdownRequested);
        assert(requested);
    }
    cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void DefaultAudioOutput::process_command(ConfigureCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        const bool requested = transition_session_locked(SessionEvent::ConfigureRequested);
        if (!requested) {
            command.completion.set_value(
                std::unexpected(invalid_state("audio output is already configured")));
            return;
        }
    }

    if (!backend_ || !audio_frame_source_ || !notifier_ || !realtime_notifier_ || !generation_) {
        std::lock_guard lock(mutex_);
        const bool failed = transition_session_locked(SessionEvent::ConfigureFailed);
        assert(failed);
        command.completion.set_value(
            std::unexpected(invalid_state("audio output dependencies are unavailable")));
        return;
    }

    if (!realtime_notifier_->seal()) {
        std::lock_guard lock(mutex_);
        const bool failed = transition_session_locked(SessionEvent::ConfigureFailed);
        assert(failed);
        command.completion.set_value(
            std::unexpected(invalid_state("audio output real-time notifier is already sealed")));
        return;
    }

    std::expected<AudioOutputConfigureResult, AudioOutputBackendError> configured;
    try {
        configured = backend_->configure(command.options);
    } catch (...) {
        configured = std::unexpected(AudioOutputBackendError{
            .operation = AudioOutputBackendOperation::Configure,
            .native_code = 0,
            .message = "audio output backend configuration threw an exception",
        });
    }
    if (!configured) {
        backend_->unconfigure();
        (void)realtime_notifier_->unseal();
        std::lock_guard lock(mutex_);
        const bool failed = transition_session_locked(SessionEvent::ConfigureFailed);
        assert(failed);
        command.completion.set_value(std::unexpected(AudioOutputError{
            .code = AudioOutputErrorCode::BackendFailure,
            .message = configured.error().message,
            .backend_error = std::move(configured.error()),
        }));
        return;
    }

    std::lock_guard lock(mutex_);
    active_generation_ = generation_->current();
    playback_clock_.configure(configured->playback_format.sample_rate);
    pending_frame_.reset();
    playback_clock_.reset();
    phase_ = PlaybackPhase::Running;
    playback_enabled_ = false;
    discarding_stale_generation_ = false;
    input_not_empty_hint_ = true;
    backend_progress_hint_ = true;
    const bool succeeded = transition_session_locked(SessionEvent::ConfigureSucceeded);
    assert(succeeded);
    command.completion.set_value(*configured);
}

void DefaultAudioOutput::process_command(UnconfigureCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::Constructed) {
            command.completion.set_value();
            return;
        }
        const bool requested = transition_session_locked(SessionEvent::UnconfigureRequested);
        assert(requested);
    }

    if (backend_) {
        backend_->unconfigure();
    }
    if (realtime_notifier_ && realtime_notifier_->sealed()) {
        (void)realtime_notifier_->unseal();
    }

    playback_clock_.reset();
    std::lock_guard lock(mutex_);
    pending_frame_.reset();
    active_generation_ = 0;
    phase_ = PlaybackPhase::Running;
    playback_enabled_ = false;
    discarding_stale_generation_ = false;
    input_not_empty_hint_ = false;
    backend_progress_hint_ = false;
    const bool succeeded = transition_session_locked(SessionEvent::UnconfigureSucceeded);
    assert(succeeded);
    command.completion.set_value();
}

void DefaultAudioOutput::process_command(StartPlaybackCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            command.completion.set_value(
                std::unexpected(invalid_state("audio output is not configured")));
            return;
        }
    }

    std::expected<void, AudioOutputBackendError> resumed;
    try {
        resumed = backend_->resume();
    } catch (...) {
        resumed = std::unexpected(backend_exception(
            AudioOutputBackendOperation::Resume,
            "audio output backend resume threw an exception"));
    }
    if (!resumed) {
        command.completion.set_value(std::unexpected(backend_failure(std::move(resumed.error()))));
        return;
    }

    playback_clock_.resume();
    std::lock_guard lock(mutex_);
    playback_enabled_ = true;
    input_not_empty_hint_ = true;
    backend_progress_hint_ = true;
    command.completion.set_value({});
    cv_.notify_one();
}

void DefaultAudioOutput::process_command(PausePlaybackCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            command.completion.set_value({});
            return;
        }
    }

    std::expected<void, AudioOutputBackendError> paused;
    try {
        paused = backend_->pause();
    } catch (...) {
        paused = std::unexpected(backend_exception(
            AudioOutputBackendOperation::Pause,
            "audio output backend pause threw an exception"));
    }
    if (!paused) {
        command.completion.set_value(std::unexpected(backend_failure(std::move(paused.error()))));
        return;
    }

    std::lock_guard lock(mutex_);
    playback_clock_.pause();
    playback_enabled_ = false;
    command.completion.set_value({});
}

bool DefaultAudioOutput::should_process_data_locked() const noexcept {
    if (session_state_ != SessionState::Configured) {
        return false;
    }

    if (generation_ && generation_->current() != active_generation_) {
        return true;
    }

    if (discarding_stale_generation_ && input_not_empty_hint_) {
        return true;
    }

    if (!playback_enabled_) {
        return false;
    }

    if (pending_frame_.has_value()) {
        return backend_progress_hint_;
    }

    if (phase_ == PlaybackPhase::Draining) {
        return backend_progress_hint_;
    }

    if (phase_ == PlaybackPhase::Finished) {
        return false;
    }

    return input_not_empty_hint_;
}

void DefaultAudioOutput::handle_generation_change_if_needed() noexcept {
    const Generation::Value observed_generation = generation_ ? generation_->current() : 0;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
        if (observed_generation == active_generation_) {
            return;
        }
    }

    if (backend_ && !reset_backend_for_generation(observed_generation)) {
        return;
    }

    const Generation::Value current_generation = generation_ ? generation_->current() : 0;
    std::lock_guard lock(mutex_);
    if (session_state_ != SessionState::Configured) {
        return;
    }
    pending_frame_.reset();
    playback_clock_.reset();
    phase_ = PlaybackPhase::Running;
    active_generation_ = current_generation;
    input_not_empty_hint_ = true;
    backend_progress_hint_ = true;
    discarding_stale_generation_ = true;
}

bool DefaultAudioOutput::reset_backend_for_generation(
    Generation::Value generation) noexcept {
    std::expected<void, AudioOutputBackendError> reset;
    try {
        reset = backend_->reset();
    } catch (...) {
        reset = std::unexpected(backend_exception(
            AudioOutputBackendOperation::Reset,
            "audio output backend reset threw an exception"));
    }
    if (!reset) {
        handle_backend_failure(std::move(reset.error()), generation);
        return false;
    }
    return true;
}

DefaultAudioOutput::DataStepResult DefaultAudioOutput::try_submit_pending_frame() noexcept {
    std::shared_ptr<AudioOutputBackend> backend;
    std::optional<AudioFrame> pending_frame;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return DataStepResult::Handled;
        }
        if (!pending_frame_.has_value()) {
            return DataStepResult::NoPendingFrame;
        }
        if (!backend_progress_hint_) {
            return DataStepResult::Handled;
        }

        backend_progress_hint_ = false;
        pending_frame.emplace(std::move(*pending_frame_));
        pending_frame_.reset();
        backend = backend_;
    }

    assert(backend);
    std::expected<AudioOutputSubmitStatus, AudioOutputBackendError> submitted;
    try {
        submitted = backend->try_submit(pending_frame->decoded());
    } catch (...) {
        submitted = std::unexpected(backend_exception(
            AudioOutputBackendOperation::Submit,
            "audio output backend submit threw an exception"));
    }

    if (!submitted) {
        handle_backend_failure(std::move(submitted.error()));
        return DataStepResult::Handled;
    }

    std::lock_guard lock(mutex_);
    if (worker_state_ == WorkerState::ShuttingDown ||
        session_state_ != SessionState::Configured) {
        return DataStepResult::Handled;
    }

    if (*submitted == AudioOutputSubmitStatus::WouldBlock) {
        pending_frame_.emplace(std::move(*pending_frame));
        return DataStepResult::Handled;
    }

    if (phase_ == PlaybackPhase::Running) {
        input_not_empty_hint_ = true;
    } else if (phase_ == PlaybackPhase::Draining) {
        backend_progress_hint_ = true;
    }
    return DataStepResult::Handled;
}

DefaultAudioOutput::DataStepResult DefaultAudioOutput::try_drain_backend() noexcept {
    std::shared_ptr<AudioOutputBackend> backend;
    Generation::Value generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return DataStepResult::Handled;
        }
        if (pending_frame_.has_value() || phase_ != PlaybackPhase::Draining) {
            return DataStepResult::NoPendingFrame;
        }
        if (!backend_progress_hint_) {
            return DataStepResult::Handled;
        }

        backend_progress_hint_ = false;
        backend = backend_;
        generation = active_generation_;
    }

    assert(backend);
    std::expected<AudioOutputDrainStatus, AudioOutputBackendError> drained;
    try {
        drained = backend->try_drain();
    } catch (...) {
        drained = std::unexpected(backend_exception(
            AudioOutputBackendOperation::Drain,
            "audio output backend drain threw an exception"));
    }

    if (!drained) {
        handle_backend_failure(std::move(drained.error()));
        return DataStepResult::Handled;
    }

    bool should_notify_finished = false;
    {
        std::lock_guard lock(mutex_);
        if (worker_state_ == WorkerState::ShuttingDown ||
            session_state_ != SessionState::Configured ||
            active_generation_ != generation) {
            return DataStepResult::Handled;
        }

        if (*drained == AudioOutputDrainStatus::WouldBlock) {
            return DataStepResult::Handled;
        }

        phase_ = PlaybackPhase::Finished;
        should_notify_finished = true;
    }

    if (should_notify_finished) {
        playback_clock_.finish();
    }

    if (should_notify_finished) {
        notify_playback_finished(generation);
    }
    return DataStepResult::Handled;
}

void DefaultAudioOutput::read_next_input_to_pending() noexcept {
    std::shared_ptr<AudioFrameSource> audio_frame_source;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured ||
            phase_ != PlaybackPhase::Running ||
            pending_frame_.has_value() ||
            (!playback_enabled_ && !discarding_stale_generation_)) {
            return;
        }
        if (!input_not_empty_hint_) {
            return;
        }

        input_not_empty_hint_ = false;
        audio_frame_source = audio_frame_source_;
    }

    assert(audio_frame_source);
    auto item = audio_frame_source->try_pop();
    if (!item) {
        return;
    }

    handle_input_item(std::move(*item));
}

void DefaultAudioOutput::handle_input_item(AudioFrameStoreItem item) noexcept {
    // try_pop() is destructive; resynchronize before accepting the item.
    handle_generation_change_if_needed();
    const Generation::Value current_generation = generation_ ? generation_->current() : 0;
    const auto item_generation = audio_frame_store_item_generation(item);
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
        if (item_generation != current_generation) {
            if (phase_ == PlaybackPhase::Running) {
                input_not_empty_hint_ = true;
            }
            return;
        }
        discarding_stale_generation_ = false;
    }

    if (auto* frame = std::get_if<AudioFrame>(&item)) {
        handle_audio_frame(std::move(*frame), current_generation);
        return;
    }

    handle_end_of_input(current_generation);
}

void DefaultAudioOutput::handle_audio_frame(
    AudioFrame frame, Generation::Value current_generation) noexcept {
    std::lock_guard lock(mutex_);
    if (worker_state_ == WorkerState::ShuttingDown ||
        session_state_ != SessionState::Configured ||
        active_generation_ != current_generation ||
        phase_ != PlaybackPhase::Running ||
        pending_frame_.has_value()) {
        return;
    }

    pending_frame_.emplace(std::move(frame));
    if (pending_frame_->decoded().pts_us) {
        (void)playback_clock_.prepare_pcm(current_generation, *pending_frame_->decoded().pts_us);
    }
    backend_progress_hint_ = true;
}

void DefaultAudioOutput::handle_end_of_input(Generation::Value generation) noexcept {
    std::lock_guard lock(mutex_);
    if (worker_state_ == WorkerState::ShuttingDown ||
        session_state_ != SessionState::Configured ||
        active_generation_ != generation ||
        phase_ != PlaybackPhase::Running ||
        pending_frame_.has_value()) {
        return;
    }

    phase_ = PlaybackPhase::Draining;
    backend_progress_hint_ = true;
}

void DefaultAudioOutput::handle_backend_failure(
    AudioOutputBackendError error,
    std::optional<Generation::Value> generation_override) noexcept {
    bool should_notify = false;
    Generation::Value generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (worker_state_ != WorkerState::ShuttingDown &&
            session_state_ == SessionState::Configured) {
            generation = generation_override.value_or(active_generation_);
            pending_frame_.reset();
            input_not_empty_hint_ = false;
            backend_progress_hint_ = false;
            discarding_stale_generation_ = false;
            const bool failed = transition_session_locked(SessionEvent::BackendFailed);
            assert(failed);
            should_notify = true;
        }
    }

    if (should_notify) {
        notify_backend_failure(std::move(error), generation);
    }
}

void DefaultAudioOutput::notify_backend_failure(AudioOutputBackendError error,
                                                Generation::Value generation) noexcept {
    if (!notifier_) {
        return;
    }

    try {
        AudioOutputBackendFailure event{.generation = generation, .error = std::move(error)};
        (void)notifier_->send(event);
    } catch (...) {
        // Backend failure is already reflected in the session state.
    }
}

void DefaultAudioOutput::notify_playback_finished(Generation::Value generation) noexcept {
    if (!notifier_) {
        return;
    }

    try {
        AudioPlaybackFinished event{.generation = generation};
        (void)notifier_->send(event);
    } catch (...) {
        // Finished phase is already reflected in the session state.
    }
}

bool DefaultAudioOutput::transition_worker_locked(WorkerEvent event) noexcept {
    switch (event) {
    case WorkerEvent::Started:
        if (worker_state_ == WorkerState::Starting) {
            worker_state_ = WorkerState::Alive;
            return true;
        }
        return false;
    case WorkerEvent::ShutdownRequested:
        if (worker_state_ == WorkerState::Starting ||
            worker_state_ == WorkerState::Alive) {
            worker_state_ = WorkerState::ShuttingDown;
            return true;
        }
        return false;
    case WorkerEvent::Stopped:
        if (worker_state_ == WorkerState::ShuttingDown) {
            worker_state_ = WorkerState::Stopped;
            return true;
        }
        return false;
    }
    return false;
}

bool DefaultAudioOutput::transition_session_locked(SessionEvent event) noexcept {
    switch (event) {
    case SessionEvent::ConfigureRequested:
        if (session_state_ == SessionState::Constructed) {
            session_state_ = SessionState::Configuring;
            return true;
        }
        return false;
    case SessionEvent::ConfigureSucceeded:
        if (session_state_ == SessionState::Configuring) {
            session_state_ = SessionState::Configured;
            return true;
        }
        return false;
    case SessionEvent::ConfigureFailed:
        if (session_state_ == SessionState::Configuring) {
            session_state_ = SessionState::Constructed;
            return true;
        }
        return false;
    case SessionEvent::UnconfigureRequested:
        if (session_state_ == SessionState::Configured ||
            session_state_ == SessionState::Failed) {
            session_state_ = SessionState::Unconfiguring;
            return true;
        }
        return false;
    case SessionEvent::UnconfigureSucceeded:
        if (session_state_ == SessionState::Unconfiguring) {
            session_state_ = SessionState::Constructed;
            return true;
        }
        return false;
    case SessionEvent::BackendFailed:
        if (session_state_ == SessionState::Configured) {
            session_state_ = SessionState::Failed;
            return true;
        }
        return false;
    }
    return false;
}

} // namespace semi::domain
