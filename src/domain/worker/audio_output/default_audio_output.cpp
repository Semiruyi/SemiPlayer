#include "domain/worker/audio_output/default_audio_output.hpp"

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

} // namespace

DefaultAudioOutput::DefaultAudioOutput(std::shared_ptr<AudioFrameSource> audio_frame_source,
                                       std::shared_ptr<AudioOutputBackend> backend,
                                       std::shared_ptr<infra::Notifier> notifier,
                                       std::shared_ptr<Generation> generation)
    : audio_frame_source_(std::move(audio_frame_source)),
      backend_(std::move(backend)),
      notifier_(std::move(notifier)),
      generation_(std::move(generation)),
      worker_([this] {
          worker_main();
      }) {
    if (backend_) {
        backend_->set_progress_notifier(this);
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
}

DefaultAudioOutput::~DefaultAudioOutput() {
    shutdown_worker();
    audio_frame_store_not_empty_subscription_.reset();
    if (backend_) {
        backend_->set_progress_notifier(nullptr);
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

void DefaultAudioOutput::pause_playback() noexcept {
    PausePlaybackCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    completion.wait();
}

void DefaultAudioOutput::notify_audio_output_progress_available() noexcept {
    {
        std::lock_guard lock(mutex_);
        backend_progress_hint_ = true;
    }
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

    if (!backend_ || !audio_frame_source_ || !notifier_ || !generation_) {
        std::lock_guard lock(mutex_);
        const bool failed = transition_session_locked(SessionEvent::ConfigureFailed);
        assert(failed);
        command.completion.set_value(
            std::unexpected(invalid_state("audio output dependencies are unavailable")));
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
    pending_frame_.reset();
    phase_ = PlaybackPhase::Running;
    playback_enabled_ = false;
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

    std::lock_guard lock(mutex_);
    pending_frame_.reset();
    active_generation_ = 0;
    phase_ = PlaybackPhase::Running;
    playback_enabled_ = false;
    input_not_empty_hint_ = false;
    backend_progress_hint_ = false;
    const bool succeeded = transition_session_locked(SessionEvent::UnconfigureSucceeded);
    assert(succeeded);
    command.completion.set_value();
}

void DefaultAudioOutput::process_command(StartPlaybackCommand& command) noexcept {
    std::lock_guard lock(mutex_);
    if (session_state_ != SessionState::Configured) {
        command.completion.set_value(
            std::unexpected(invalid_state("audio output is not configured")));
        return;
    }

    playback_enabled_ = true;
    input_not_empty_hint_ = true;
    backend_progress_hint_ = true;
    command.completion.set_value({});
    cv_.notify_one();
}

void DefaultAudioOutput::process_command(PausePlaybackCommand& command) noexcept {
    std::lock_guard lock(mutex_);
    if (session_state_ == SessionState::Configured) {
        playback_enabled_ = false;
    }
    command.completion.set_value();
}

bool DefaultAudioOutput::should_process_data_locked() const noexcept {
    if (session_state_ != SessionState::Configured) {
        return false;
    }

    if (generation_ && generation_->current() != active_generation_) {
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
    const Generation::Value current_generation = generation_ ? generation_->current() : 0;
    bool generation_changed = false;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
        if (current_generation != active_generation_) {
            pending_frame_.reset();
            phase_ = PlaybackPhase::Running;
            active_generation_ = current_generation;
            input_not_empty_hint_ = true;
            backend_progress_hint_ = true;
            generation_changed = true;
        }
    }

    if (generation_changed && backend_) {
        backend_->reset();
    }
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
            pending_frame_.has_value()) {
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
    const Generation::Value current_generation = generation_ ? generation_->current() : 0;
    bool generation_changed = false;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
        if (current_generation != active_generation_) {
            pending_frame_.reset();
            phase_ = PlaybackPhase::Running;
            active_generation_ = current_generation;
            generation_changed = true;
        }
    }

    if (generation_changed && backend_) {
        backend_->reset();
    }

    if (audio_frame_store_item_generation(item) != current_generation) {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::Configured &&
            phase_ == PlaybackPhase::Running) {
            input_not_empty_hint_ = true;
        }
        return;
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

void DefaultAudioOutput::handle_backend_failure(AudioOutputBackendError error) noexcept {
    bool should_notify = false;
    Generation::Value generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (worker_state_ != WorkerState::ShuttingDown &&
            session_state_ == SessionState::Configured) {
            generation = active_generation_;
            pending_frame_.reset();
            input_not_empty_hint_ = false;
            backend_progress_hint_ = false;
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
