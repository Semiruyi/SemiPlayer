#include "domain/worker/audio_decoder/default_audio_decoder.hpp"

#include <cassert>
#include <string>
#include <utility>

namespace semi::domain {
namespace {

AudioDecoderError invalid_state(std::string message) {
    return AudioDecoderError{
        .code = AudioDecoderErrorCode::InvalidState,
        .message = std::move(message),
        .backend_error = std::nullopt,
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
            input_not_empty_hint_.store(true, std::memory_order_release);
            cv_.notify_one();
        });
    audio_frame_store_not_full_subscription_ = notifier_->subscribe<AudioFrameStoreNotFull>(
        [this](const AudioFrameStoreNotFull&) {
            output_not_full_hint_.store(true, std::memory_order_release);
            cv_.notify_one();
        });
}

DefaultAudioDecoder::~DefaultAudioDecoder() {
    shutdown_worker();
    audio_queue_not_empty_subscription_.reset();
    audio_frame_store_not_full_subscription_.reset();
}

std::expected<void, AudioDecoderError> DefaultAudioDecoder::configure(
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
            return worker_state_ == WorkerState::ShuttingDown || !commands_.empty();
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
        // 后续步骤：Configured 时在此接入数据面分支（should_process_data_locked）。
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

    std::expected<void, AudioDecoderBackendError> configured;
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

    // 建立新会话上下文：捕获当前世代，清空上一会话残留。
    active_generation_ = generation_->current();
    pending_outputs_.clear();
    input_exhausted_ = false;
    input_not_empty_hint_.store(true, std::memory_order_release);
    output_not_full_hint_.store(true, std::memory_order_release);

    std::lock_guard lock(mutex_);
    const bool succeeded = transition_session_locked(SessionEvent::ConfigureSucceeded);
    assert(succeeded);
    command.completion.set_value({});
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

    pending_outputs_.clear();
    active_generation_ = 0;
    input_exhausted_ = false;
    input_not_empty_hint_.store(false, std::memory_order_release);
    output_not_full_hint_.store(false, std::memory_order_release);

    std::lock_guard lock(mutex_);
    const bool succeeded = transition_session_locked(SessionEvent::UnconfigureSucceeded);
    assert(succeeded);
    command.completion.set_value();
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
