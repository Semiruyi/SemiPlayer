#include "domain/worker/audio_resampler/default_audio_resampler.hpp"

#include "domain/resource/generation/generation_events.hpp"
#include "domain/worker/audio_resampler/audio_resampler_events.hpp"

#include <cassert>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace semi::domain {
namespace {

void require_state_transition(bool succeeded) noexcept {
    assert(succeeded);
    if (!succeeded) [[unlikely]] {
        std::terminate();
    }
}

AudioResamplerError invalid_state(std::string message) {
    return AudioResamplerError{
        .code = AudioResamplerErrorCode::InvalidState,
        .message = std::move(message),
        .backend_error = std::nullopt,
    };
}

AudioResamplerBackendError backend_exception(AudioResamplerBackendOperation operation,
                                             std::string message) {
    return AudioResamplerBackendError{
        .operation = operation,
        .native_code = 0,
        .message = std::move(message),
    };
}

} // namespace

DefaultAudioResampler::DefaultAudioResampler(
    std::shared_ptr<AudioFrameSource> audio_frame_source,
    std::shared_ptr<AudioFrameSink> audio_frame_sink,
    std::shared_ptr<AudioResamplerBackend> backend,
    std::shared_ptr<infra::Notifier> notifier,
    std::shared_ptr<Generation> generation)
    : audio_frame_source_(std::move(audio_frame_source)),
      audio_frame_sink_(std::move(audio_frame_sink)),
      backend_(std::move(backend)),
      notifier_(std::move(notifier)),
      generation_(std::move(generation)),
      worker_([this] {
          worker_main();
      }) {
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
    audio_frame_store_not_full_subscription_ = notifier_->subscribe<AudioFrameStoreNotFull>(
        [this](const AudioFrameStoreNotFull&) {
            {
                std::lock_guard lock(mutex_);
                output_not_full_hint_ = true;
            }
            cv_.notify_one();
        });
    generation_changed_subscription_ = notifier_->subscribe<GenerationChanged>(
        [this](const GenerationChanged&) {
            cv_.notify_one();
        });
}

DefaultAudioResampler::~DefaultAudioResampler() {
    shutdown_worker();
    audio_frame_store_not_empty_subscription_.reset();
    audio_frame_store_not_full_subscription_.reset();
    generation_changed_subscription_.reset();
}

std::expected<void, AudioResamplerError> DefaultAudioResampler::configure(
    const contracts::media::AudioPcmFormat& input_format,
    const contracts::media::AudioPcmFormat& output_format) {
    ConfigureCommand command;
    command.input_format = input_format;
    command.output_format = output_format;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

void DefaultAudioResampler::unconfigure() noexcept {
    UnconfigureCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    completion.wait();
}

void DefaultAudioResampler::worker_main() noexcept {
    std::unique_lock lock(mutex_);
    if (worker_state_ == WorkerState::Starting) {
        require_state_transition(transition_worker_locked(WorkerEvent::Started));
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
            adopt_generation_if_needed(generation_ ? generation_->current() : 0);
            if (try_push_pending_output() == PendingOutputPushResult::NoPending) {
                read_next_input_to_pending();
            }
            lock.lock();
        }
    }
    require_state_transition(transition_worker_locked(WorkerEvent::Stopped));
    cv_.notify_all();
}

void DefaultAudioResampler::shutdown_worker() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (worker_state_ == WorkerState::Stopped) {
            return;
        }
        require_state_transition(transition_worker_locked(WorkerEvent::ShutdownRequested));
    }
    cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void DefaultAudioResampler::process_command(ConfigureCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        const bool requested = transition_session_locked(SessionEvent::ConfigureRequested);
        if (!requested) {
            command.completion.set_value(
                std::unexpected(invalid_state("audio resampler is already configured")));
            return;
        }
    }

    if (!backend_ || !audio_frame_source_ || !audio_frame_sink_ || !notifier_ || !generation_) {
        std::lock_guard lock(mutex_);
        require_state_transition(transition_session_locked(SessionEvent::ConfigureFailed));
        command.completion.set_value(
            std::unexpected(invalid_state("audio resampler dependencies are unavailable")));
        return;
    }

    std::expected<void, AudioResamplerBackendError> configured;
    try {
        configured = backend_->configure(command.input_format, command.output_format);
    } catch (...) {
        configured = std::unexpected(AudioResamplerBackendError{
            .operation = AudioResamplerBackendOperation::Configure,
            .native_code = 0,
            .message = "audio resampler backend configuration threw an exception",
        });
    }
    if (!configured) {
        backend_->unconfigure();
        std::lock_guard lock(mutex_);
        require_state_transition(transition_session_locked(SessionEvent::ConfigureFailed));
        command.completion.set_value(std::unexpected(AudioResamplerError{
            .code = AudioResamplerErrorCode::BackendFailure,
            .message = configured.error().message,
            .backend_error = std::move(configured.error()),
        }));
        return;
    }

    std::lock_guard lock(mutex_);
    active_generation_ = generation_->current();
    pending_outputs_.clear();
    input_exhausted_ = false;
    input_not_empty_hint_ = true;
    output_not_full_hint_ = true;
    require_state_transition(transition_session_locked(SessionEvent::ConfigureSucceeded));
    command.completion.set_value({});
}

void DefaultAudioResampler::process_command(UnconfigureCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::Constructed) {
            command.completion.set_value();
            return;
        }
        require_state_transition(transition_session_locked(SessionEvent::UnconfigureRequested));
    }

    if (backend_) {
        backend_->unconfigure();
    }

    std::lock_guard lock(mutex_);
    pending_outputs_.clear();
    active_generation_ = 0;
    input_exhausted_ = false;
    input_not_empty_hint_ = false;
    output_not_full_hint_ = false;
    require_state_transition(transition_session_locked(SessionEvent::UnconfigureSucceeded));
    command.completion.set_value();
}

bool DefaultAudioResampler::should_process_data_locked() const noexcept {
    if (session_state_ != SessionState::Configured) {
        return false;
    }

    if (generation_ && active_generation_ != generation_->current()) {
        return true;
    }

    if (!pending_outputs_.empty()) {
        return output_not_full_hint_;
    }

    if (input_exhausted_ &&
        (!generation_ || active_generation_ == generation_->current())) {
        return false;
    }

    return input_not_empty_hint_;
}

void DefaultAudioResampler::adopt_generation_if_needed(
    Generation::Value current_generation) noexcept {
    bool generation_changed = false;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured || current_generation == active_generation_) {
            return;
        }

        pending_outputs_.clear();
        input_exhausted_ = false;
        active_generation_ = current_generation;
        input_not_empty_hint_ = true;
        output_not_full_hint_ = false;
        generation_changed = true;
    }

    if (generation_changed && backend_) {
        backend_->reset();
    }
}

DefaultAudioResampler::PendingOutputPushResult
DefaultAudioResampler::try_push_pending_output() noexcept {
    std::shared_ptr<AudioFrameSink> audio_frame_sink;
    std::optional<AudioFrameStoreItem> pending_output;

    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return PendingOutputPushResult::Handled;
        }
        if (pending_outputs_.empty()) {
            return PendingOutputPushResult::NoPending;
        }
        if (!output_not_full_hint_) {
            return PendingOutputPushResult::Handled;
        }

        output_not_full_hint_ = false;
        pending_output.emplace(std::move(pending_outputs_.front()));
        pending_outputs_.pop_front();
        audio_frame_sink = audio_frame_sink_;
    }

    assert(audio_frame_sink);
    const auto pushed = audio_frame_sink->try_push(std::move(*pending_output));

    std::lock_guard lock(mutex_);
    if (worker_state_ == WorkerState::ShuttingDown ||
        session_state_ != SessionState::Configured) {
        return PendingOutputPushResult::Handled;
    }

    if (pushed == AudioFramePushResult::Full) {
        pending_outputs_.push_front(std::move(*pending_output));
        return PendingOutputPushResult::Handled;
    }

    if (!pending_outputs_.empty()) {
        output_not_full_hint_ = true;
    } else if (!input_exhausted_) {
        input_not_empty_hint_ = true;
    }
    return PendingOutputPushResult::Handled;
}

void DefaultAudioResampler::read_next_input_to_pending() noexcept {
    std::shared_ptr<AudioFrameSource> audio_frame_source;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured ||
            (input_exhausted_ &&
             (!generation_ || active_generation_ == generation_->current())) ||
            !pending_outputs_.empty()) {
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

void DefaultAudioResampler::handle_input_item(AudioFrameStoreItem item) noexcept {
    const Generation::Value current_generation = generation_ ? generation_->current() : 0;
    adopt_generation_if_needed(current_generation);

    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
    }

    if (audio_frame_store_item_generation(item) != current_generation) {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::Configured && !input_exhausted_) {
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

void DefaultAudioResampler::handle_audio_frame(
    AudioFrame frame, Generation::Value current_generation) noexcept {
    std::shared_ptr<AudioResamplerBackend> backend;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
        backend = backend_;
    }

    assert(backend);
    std::expected<contracts::audio_resampler::ResampledAudioBatch, AudioResamplerBackendError>
        resampled;
    try {
        resampled = backend->resample(frame.decoded());
    } catch (...) {
        resampled = std::unexpected(backend_exception(
            AudioResamplerBackendOperation::Resample,
            "audio resampler backend resample threw an exception"));
    }

    if (!resampled) {
        handle_backend_failure(std::move(resampled.error()));
        return;
    }

    store_resampled_outputs(std::move(*resampled), current_generation, false);
}

void DefaultAudioResampler::handle_end_of_input(Generation::Value generation) noexcept {
    std::shared_ptr<AudioResamplerBackend> backend;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
        input_exhausted_ = true;
        backend = backend_;
    }

    assert(backend);
    std::expected<contracts::audio_resampler::ResampledAudioBatch, AudioResamplerBackendError>
        drained;
    try {
        drained = backend->drain();
    } catch (...) {
        drained = std::unexpected(backend_exception(
            AudioResamplerBackendOperation::Drain,
            "audio resampler backend drain threw an exception"));
    }

    if (!drained) {
        handle_backend_failure(std::move(drained.error()));
        return;
    }

    store_resampled_outputs(std::move(*drained), generation, true);
}

void DefaultAudioResampler::store_resampled_outputs(
    contracts::audio_resampler::ResampledAudioBatch resampled,
    Generation::Value generation,
    bool append_end_of_input) noexcept {
    std::lock_guard lock(mutex_);
    if (worker_state_ == WorkerState::ShuttingDown ||
        session_state_ != SessionState::Configured ||
        active_generation_ != generation) {
        return;
    }

    for (auto& frame : resampled) {
        pending_outputs_.emplace_back(std::in_place_type<AudioFrame>, std::move(frame), generation);
    }
    if (append_end_of_input) {
        pending_outputs_.emplace_back(AudioFrameEndOfInput{.generation = generation});
    }

    if (!pending_outputs_.empty()) {
        output_not_full_hint_ = true;
    } else if (!input_exhausted_) {
        input_not_empty_hint_ = true;
    }
}

void DefaultAudioResampler::handle_backend_failure(AudioResamplerBackendError error) noexcept {
    bool should_notify = false;
    {
        std::lock_guard lock(mutex_);
        if (worker_state_ != WorkerState::ShuttingDown &&
            session_state_ == SessionState::Configured) {
            pending_outputs_.clear();
            input_not_empty_hint_ = false;
            output_not_full_hint_ = false;
            require_state_transition(transition_session_locked(SessionEvent::BackendFailed));
            should_notify = true;
        }
    }

    if (should_notify) {
        notify_backend_failure(std::move(error));
    }
}

void DefaultAudioResampler::notify_backend_failure(AudioResamplerBackendError error) noexcept {
    if (!notifier_) {
        return;
    }

    try {
        AudioResamplerBackendFailure event{.error = std::move(error)};
        (void)notifier_->send(event);
    } catch (...) {
        // Backend failure is already reflected in the session state.
    }
}

bool DefaultAudioResampler::transition_worker_locked(WorkerEvent event) noexcept {
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

bool DefaultAudioResampler::transition_session_locked(SessionEvent event) noexcept {
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
