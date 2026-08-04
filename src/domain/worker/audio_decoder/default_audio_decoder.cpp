#include "domain/worker/audio_decoder/default_audio_decoder.hpp"

#include "domain/worker/audio_decoder/audio_decoder_events.hpp"

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace semi::domain {
namespace {

AudioDecoderError invalid_state(std::string message) {
    return AudioDecoderError{
        .code = AudioDecoderErrorCode::InvalidState,
        .message = std::move(message),
        .backend_error = std::nullopt,
    };
}

AudioDecoderBackendError backend_exception(AudioDecoderBackendOperation operation,
                                           std::string message) {
    return AudioDecoderBackendError{
        .operation = operation,
        .native_code = 0,
        .message = std::move(message),
    };
}

} // namespace

DefaultAudioDecoder::DefaultAudioDecoder(
    std::shared_ptr<AudioPacketSource> audio_packet_source,
    std::shared_ptr<AudioFrameSink> audio_frame_sink,
    std::shared_ptr<AudioDecoderBackend> backend,
    std::shared_ptr<infra::Notifier> notifier,
    std::shared_ptr<Generation> generation)
    : audio_packet_source_(std::move(audio_packet_source)),
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

    audio_queue_not_empty_subscription_ = notifier_->subscribe<AudioQueueNotEmpty>(
        [this](const AudioQueueNotEmpty&) {
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
}

DefaultAudioDecoder::~DefaultAudioDecoder() {
    shutdown_worker();
    audio_queue_not_empty_subscription_.reset();
    audio_frame_store_not_full_subscription_.reset();
}

std::expected<AudioDecoderConfigureResult, AudioDecoderError> DefaultAudioDecoder::configure(
    const contracts::media::AudioCodecConfig& config) {
    ConfigureCommand command;
    command.config = config;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

void DefaultAudioDecoder::unconfigure() noexcept {
    UnconfigureCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    completion.wait();
}

void DefaultAudioDecoder::worker_main() noexcept {
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
            if (try_push_pending_output() == PendingOutputPushResult::NoPending) {
                read_next_input_to_pending();
            }
            lock.lock();
        }
    }
    const bool stopped = transition_worker_locked(WorkerEvent::Stopped);
    assert(stopped);
    cv_.notify_all();
}

void DefaultAudioDecoder::shutdown_worker() noexcept {
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

void DefaultAudioDecoder::process_command(ConfigureCommand& command) noexcept {
    {
        std::lock_guard lock(mutex_);
        const bool requested = transition_session_locked(SessionEvent::ConfigureRequested);
        if (!requested) {
            command.completion.set_value(
                std::unexpected(invalid_state("audio decoder is already configured")));
            return;
        }
    }

    if (!backend_ || !audio_packet_source_ || !audio_frame_sink_ || !notifier_ || !generation_) {
        std::lock_guard lock(mutex_);
        const bool failed = transition_session_locked(SessionEvent::ConfigureFailed);
        assert(failed);
        command.completion.set_value(
            std::unexpected(invalid_state("audio decoder dependencies are unavailable")));
        return;
    }

    std::expected<contracts::audio_decoder::AudioDecoderBackendConfigureResult,
                  AudioDecoderBackendError>
        configured;
    try {
        configured = backend_->configure(command.config);
    } catch (...) {
        configured = std::unexpected(AudioDecoderBackendError{
            .operation = AudioDecoderBackendOperation::Configure,
            .native_code = 0,
            .message = "audio decoder backend configuration threw an exception",
        });
    }
    if (!configured) {
        backend_->unconfigure();
        std::lock_guard lock(mutex_);
        const bool failed = transition_session_locked(SessionEvent::ConfigureFailed);
        assert(failed);
        command.completion.set_value(std::unexpected(AudioDecoderError{
            .code = AudioDecoderErrorCode::BackendFailure,
            .message = configured.error().message,
            .backend_error = std::move(configured.error()),
        }));
        return;
    }

    std::lock_guard lock(mutex_);
    // 建立新会话上下文：捕获当前世代，清空上一会话残留。
    active_generation_ = generation_->current();
    pending_outputs_.clear();
    input_exhausted_ = false;
    input_not_empty_hint_ = true;
    output_not_full_hint_ = true;
    const bool succeeded = transition_session_locked(SessionEvent::ConfigureSucceeded);
    assert(succeeded);
    command.completion.set_value(AudioDecoderConfigureResult{
        .decoded_format = configured->decoded_format,
    });
}

void DefaultAudioDecoder::process_command(UnconfigureCommand& command) noexcept {
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
    pending_outputs_.clear();
    active_generation_ = 0;
    input_exhausted_ = false;
    input_not_empty_hint_ = false;
    output_not_full_hint_ = false;
    const bool succeeded = transition_session_locked(SessionEvent::UnconfigureSucceeded);
    assert(succeeded);
    command.completion.set_value();
}

bool DefaultAudioDecoder::should_process_data_locked() const noexcept {
    if (session_state_ != SessionState::Configured) {
        return false;
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

DefaultAudioDecoder::PendingOutputPushResult
DefaultAudioDecoder::try_push_pending_output() noexcept {
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

void DefaultAudioDecoder::read_next_input_to_pending() noexcept {
    std::shared_ptr<AudioPacketSource> audio_packet_source;
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
        audio_packet_source = audio_packet_source_;
    }

    assert(audio_packet_source);
    auto item = audio_packet_source->try_pop();
    if (!item) {
        return;
    }

    handle_input_item(std::move(*item));
}

void DefaultAudioDecoder::handle_input_item(AudioPacketQueueItem item) noexcept {
    const Generation::Value current_generation = generation_ ? generation_->current() : 0;
    bool generation_changed = false;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
        if (current_generation != active_generation_) {
            pending_outputs_.clear();
            input_exhausted_ = false;
            active_generation_ = current_generation;
            generation_changed = true;
        }
    }

    if (generation_changed && backend_) {
        backend_->reset();
    }

    if (audio_packet_queue_item_generation(item) != current_generation) {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::Configured && !input_exhausted_) {
            input_not_empty_hint_ = true;
        }
        return;
    }

    if (auto* packet = std::get_if<AudioPacket>(&item)) {
        handle_audio_packet(std::move(*packet), current_generation);
        return;
    }

    handle_end_of_input(current_generation);
}

void DefaultAudioDecoder::handle_audio_packet(
    AudioPacket packet, Generation::Value current_generation) noexcept {
    std::shared_ptr<AudioDecoderBackend> backend;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
        backend = backend_;
    }

    assert(backend);
    std::expected<contracts::audio_decoder::DecodedAudioBatch, AudioDecoderBackendError> decoded;
    try {
        decoded = backend->decode(packet.encoded());
    } catch (...) {
        decoded = std::unexpected(backend_exception(
            AudioDecoderBackendOperation::Decode,
            "audio decoder backend decode threw an exception"));
    }

    if (!decoded) {
        handle_backend_failure(std::move(decoded.error()));
        return;
    }

    store_decoded_outputs(std::move(*decoded), current_generation, false);
}

void DefaultAudioDecoder::handle_end_of_input(Generation::Value generation) noexcept {
    std::shared_ptr<AudioDecoderBackend> backend;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::Configured) {
            return;
        }
        input_exhausted_ = true;
        backend = backend_;
    }

    assert(backend);
    std::expected<contracts::audio_decoder::DecodedAudioBatch, AudioDecoderBackendError> drained;
    try {
        drained = backend->drain();
    } catch (...) {
        drained = std::unexpected(backend_exception(
            AudioDecoderBackendOperation::Drain,
            "audio decoder backend drain threw an exception"));
    }

    if (!drained) {
        handle_backend_failure(std::move(drained.error()));
        return;
    }

    store_decoded_outputs(std::move(*drained), generation, true);
}

void DefaultAudioDecoder::store_decoded_outputs(
    contracts::audio_decoder::DecodedAudioBatch decoded,
    Generation::Value generation,
    bool append_end_of_input) noexcept {
    std::lock_guard lock(mutex_);
    if (worker_state_ == WorkerState::ShuttingDown ||
        session_state_ != SessionState::Configured ||
        active_generation_ != generation) {
        return;
    }

    for (auto& frame : decoded) {
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

void DefaultAudioDecoder::handle_backend_failure(AudioDecoderBackendError error) noexcept {
    bool should_notify = false;
    {
        std::lock_guard lock(mutex_);
        if (worker_state_ != WorkerState::ShuttingDown &&
            session_state_ == SessionState::Configured) {
            pending_outputs_.clear();
            input_not_empty_hint_ = false;
            output_not_full_hint_ = false;
            const bool failed = transition_session_locked(SessionEvent::BackendFailed);
            assert(failed);
            should_notify = true;
        }
    }

    if (should_notify) {
        notify_backend_failure(std::move(error));
    }
}

void DefaultAudioDecoder::notify_backend_failure(AudioDecoderBackendError error) noexcept {
    if (!notifier_) {
        return;
    }

    try {
        AudioDecoderBackendFailure event{.error = std::move(error)};
        (void)notifier_->send(event);
    } catch (...) {
        // Backend failure is already reflected in the session state.
    }
}

bool DefaultAudioDecoder::transition_worker_locked(WorkerEvent event) noexcept {
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

bool DefaultAudioDecoder::transition_session_locked(SessionEvent event) noexcept {
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
