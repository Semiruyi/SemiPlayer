#include "domain/worker/audio_decoder/default_audio_decoder.hpp"

#include "domain/resource/audio_frame_store/audio_frame_store_item.hpp"

#include <cassert>
#include <concepts>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace semi::domain {
namespace {

template <typename... Functions>
struct Overloaded : Functions... {
    using Functions::operator()...;
};

template <typename... Functions>
Overloaded(Functions...) -> Overloaded<Functions...>;

AudioDecoderError invalid_state(std::string message) {
    return AudioDecoderError{
        .code = AudioDecoderErrorCode::InvalidState,
        .message = std::move(message),
        .backend_error = std::nullopt,
    };
}

AudioDecoderError backend_failure(AudioDecoderBackendError error) {
    return AudioDecoderError{
        .code = AudioDecoderErrorCode::BackendFailure,
        .message = error.message,
        .backend_error = std::move(error),
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
      generation_(std::move(generation)) {
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
    unconfigure();
    audio_queue_not_empty_subscription_.reset();
    audio_frame_store_not_full_subscription_.reset();
}

std::expected<void, AudioDecoderError> DefaultAudioDecoder::configure(
    const contracts::media::AudioCodecConfig& config) {
    std::lock_guard lock(mutex_);
    if (state_ != State::Constructed) {
        return std::unexpected(invalid_state("audio decoder is already configured"));
    }
    if (!audio_packet_source_) {
        return std::unexpected(invalid_state("audio packet source is unavailable"));
    }
    if (!audio_frame_sink_) {
        return std::unexpected(invalid_state("audio frame sink is unavailable"));
    }
    if (!backend_) {
        return std::unexpected(invalid_state("audio decoder backend is unavailable"));
    }
    if (!generation_) {
        return std::unexpected(invalid_state("generation is unavailable"));
    }

    try {
        auto configured = backend_->configure(config);
        if (!configured) {
            backend_->unconfigure();
            return std::unexpected(backend_failure(std::move(configured.error())));
        }
    } catch (...) {
        backend_->unconfigure();
        return std::unexpected(
            AudioDecoderError{
                .code = AudioDecoderErrorCode::BackendFailure,
                .message = "audio decoder backend configuration threw an exception",
                .backend_error = std::nullopt,
            });
    }

    active_generation_ = generation_->current();
    pending_outputs_.clear();
    input_exhausted_ = false;
    input_not_empty_hint_.store(false, std::memory_order_release);
    output_not_full_hint_.store(false, std::memory_order_release);

    const bool configured_state = transition_locked(Event::ConfigureSucceeded);
    assert(configured_state);
    return {};
}

std::expected<void, AudioDecoderError> DefaultAudioDecoder::start() {
    std::unique_lock lock(mutex_);
    if (state_ == State::Constructed) {
        return std::unexpected(
            invalid_state("audio decoder must be configured before starting"));
    }
    if (state_ == State::Running) {
        return {};
    }
    if (state_ == State::Stopping) {
        return std::unexpected(invalid_state("audio decoder is stopping"));
    }
    if (state_ == State::Failed) {
        return std::unexpected(
            invalid_state("audio decoder failed; unconfigure before starting again"));
    }

    assert(state_ == State::Configured);
    if (worker_.joinable()) {
        std::thread stale_worker = std::move(worker_);
        lock.unlock();
        stale_worker.join();
        lock.lock();
        if (state_ != State::Configured) {
            return std::unexpected(
                invalid_state("audio decoder state changed while joining its worker"));
        }
    }

    const bool starting = transition_locked(Event::StartRequested);
    assert(starting);
    input_not_empty_hint_.store(true, std::memory_order_release);
    output_not_full_hint_.store(true, std::memory_order_release);

    try {
        worker_running_ = true;
        worker_ = std::thread([this] {
            worker_main();
        });
    } catch (...) {
        worker_running_ = false;
        const bool reset_to_configured = transition_locked(Event::WorkerStartFailed);
        assert(reset_to_configured);
        input_not_empty_hint_.store(false, std::memory_order_release);
        output_not_full_hint_.store(false, std::memory_order_release);
        return std::unexpected(
            AudioDecoderError{
                .code = AudioDecoderErrorCode::BackendFailure,
                .message = "failed to start audio decoder worker",
                .backend_error = std::nullopt,
            });
    }

    cv_.notify_one();
    return {};
}

void DefaultAudioDecoder::stop() noexcept {
    std::thread worker;
    {
        std::lock_guard lock(mutex_);
        if (state_ == State::Running) {
            const bool stopping = transition_locked(Event::StopRequested);
            assert(stopping);
        }

        if (worker_.joinable()) {
            worker = std::move(worker_);
        }
    }

    cv_.notify_one();
    if (worker.joinable()) {
        worker.join();
    }

    input_not_empty_hint_.store(false, std::memory_order_release);
    output_not_full_hint_.store(false, std::memory_order_release);
}

void DefaultAudioDecoder::unconfigure() noexcept {
    stop();

    std::lock_guard lock(mutex_);
    if (state_ == State::Constructed) {
        return;
    }

    if (backend_) {
        backend_->unconfigure();
    }
    pending_outputs_.clear();
    active_generation_ = 0;
    input_exhausted_ = false;
    input_not_empty_hint_.store(false, std::memory_order_release);
    output_not_full_hint_.store(false, std::memory_order_release);

    const bool unconfigured = transition_locked(Event::UnconfigureRequested);
    assert(unconfigured);
}

void DefaultAudioDecoder::worker_main() noexcept {
    try {
        std::optional<WorkerExit> exit;
        while (!exit) {
            synchronize_generation();

            if (!pending_outputs_.empty()) {
                if (flush_pending_outputs()) {
                    continue;
                }
            } else {
                bool input_was_read = false;
                exit = read_and_process_input(input_was_read);
                if (exit) {
                    break;
                }
                if (input_was_read) {
                    continue;
                }
            }

            switch (wait_for_work(!pending_outputs_.empty())) {
            case WorkAction::Stop:
                exit = WorkerExit::Stopped;
                break;
            case WorkAction::RetryPendingOutputs:
            case WorkAction::ReadInput:
                break;
            }
        }

        std::lock_guard lock(mutex_);
        complete_worker_locked(*exit);
    } catch (...) {
        notify_backend_failure(AudioDecoderBackendError{
            .operation = AudioDecoderBackendOperation::Decode,
            .native_code = 0,
            .message = "audio decoder worker failed",
        });
        std::lock_guard lock(mutex_);
        complete_worker_locked(WorkerExit::Failed);
    }
}

DefaultAudioDecoder::WorkAction
DefaultAudioDecoder::wait_for_work(bool has_pending_outputs) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this, has_pending_outputs] {
        if (state_ == State::Stopping) {
            return true;
        }
        if (state_ != State::Running) {
            return true;
        }
        return has_pending_outputs
                   ? output_not_full_hint_.load(std::memory_order_acquire)
                   : input_not_empty_hint_.load(std::memory_order_acquire);
    });

    if (state_ == State::Stopping || state_ != State::Running) {
        return WorkAction::Stop;
    }
    if (has_pending_outputs) {
        output_not_full_hint_.store(false, std::memory_order_release);
        return WorkAction::RetryPendingOutputs;
    }

    input_not_empty_hint_.store(false, std::memory_order_release);
    return WorkAction::ReadInput;
}

std::optional<DefaultAudioDecoder::WorkerExit>
DefaultAudioDecoder::read_and_process_input(bool& input_was_read) {
    input_was_read = false;

    auto item = audio_packet_source_->try_pop();
    if (!item) {
        input_not_empty_hint_.store(false, std::memory_order_release);
        // Recheck after clearing the hint. A producer may have crossed the
        // empty -> non-empty boundary during the first failed pop.
        item = audio_packet_source_->try_pop();
        if (!item) {
            return std::nullopt;
        }
    }

    input_was_read = true;
    input_not_empty_hint_.store(false, std::memory_order_release);

    {
        std::lock_guard lock(mutex_);
        if (state_ != State::Running) {
            return WorkerExit::Stopped;
        }
    }
    return process_input_item(std::move(*item));
}

std::optional<DefaultAudioDecoder::WorkerExit>
DefaultAudioDecoder::process_input_item(AudioPacketQueueItem&& item) {
    synchronize_generation();
    if (!is_current_audio_packet_queue_item(item, active_generation_)) {
        return std::nullopt;
    }
    if (input_exhausted_) {
        return std::nullopt;
    }

    return std::visit(
        Overloaded{
            [this](AudioPacket&& packet) {
                return process_audio_packet(std::move(packet));
            },
            [this](AudioPacketEndOfInput&& end_of_input) {
                return process_end_of_input(std::move(end_of_input));
            },
        },
        std::move(item));
}

std::optional<DefaultAudioDecoder::WorkerExit>
DefaultAudioDecoder::process_audio_packet(AudioPacket&& packet) {
    auto decoded = backend_->decode(packet.encoded());
    if (!decoded) {
        notify_backend_failure(std::move(decoded.error()));
        return WorkerExit::Failed;
    }

    append_decoded_audio(std::move(*decoded), packet.generation());
    (void)flush_pending_outputs();
    return std::nullopt;
}

std::optional<DefaultAudioDecoder::WorkerExit>
DefaultAudioDecoder::process_end_of_input(AudioPacketEndOfInput end_of_input) {
    auto drained = backend_->drain();
    if (!drained) {
        notify_backend_failure(std::move(drained.error()));
        return WorkerExit::Failed;
    }

    input_exhausted_ = true;
    append_decoded_audio(std::move(*drained), end_of_input.generation);
    pending_outputs_.emplace_back(AudioFrameEndOfInput{
        .generation = end_of_input.generation,
    });
    (void)flush_pending_outputs();
    return std::nullopt;
}

void DefaultAudioDecoder::append_decoded_audio(
    contracts::audio_decoder::DecodedAudioBatch&& decoded,
    Generation::Value generation) {
    for (auto& audio : decoded) {
        pending_outputs_.emplace_back(
            std::in_place_type<AudioFrame>,
            std::move(audio),
            generation);
    }
}

bool DefaultAudioDecoder::flush_pending_outputs() {
    while (!pending_outputs_.empty()) {
        if (generation_->current() != active_generation_) {
            synchronize_generation();
            if (pending_outputs_.empty()) {
                return true;
            }
        }

        // Clear the hint immediately before trying. If the store is still
        // full, this prevents a stale hint from causing a busy retry loop.
        output_not_full_hint_.store(false, std::memory_order_release);
        if (audio_frame_sink_->try_push(std::move(pending_outputs_.front())) ==
            AudioFramePushResult::Full) {
            return false;
        }
        pending_outputs_.pop_front();
    }
    return true;
}

void DefaultAudioDecoder::synchronize_generation() noexcept {
    const auto current_generation = generation_->current();
    if (current_generation == active_generation_) {
        return;
    }

    backend_->reset();
    pending_outputs_.clear();
    active_generation_ = current_generation;
    input_exhausted_ = false;
}

bool DefaultAudioDecoder::transition_locked(Event event) noexcept {
    switch (event) {
    case Event::ConfigureSucceeded:
        if (state_ == State::Constructed) {
            state_ = State::Configured;
            return true;
        }
        return false;
    case Event::StartRequested:
        if (state_ == State::Configured) {
            state_ = State::Running;
            return true;
        }
        return false;
    case Event::WorkerStartFailed:
        if (state_ == State::Running) {
            state_ = State::Configured;
            return true;
        }
        return false;
    case Event::StopRequested:
        if (state_ == State::Running) {
            state_ = State::Stopping;
            return true;
        }
        return state_ == State::Configured;
    case Event::WorkerStopped:
        if (state_ == State::Stopping) {
            state_ = State::Configured;
            return true;
        }
        return false;
    case Event::BackendFailed:
        if (state_ == State::Running) {
            state_ = State::Failed;
            return true;
        }
        return false;
    case Event::UnconfigureRequested:
        if (state_ == State::Configured || state_ == State::Failed) {
            state_ = State::Constructed;
            return true;
        }
        return false;
    }
    return false;
}

void DefaultAudioDecoder::complete_worker_locked(WorkerExit exit) noexcept {
    worker_running_ = false;
    if (state_ == State::Stopping) {
        const bool stopped = transition_locked(Event::WorkerStopped);
        assert(stopped);
        return;
    }

    if (exit == WorkerExit::Failed) {
        const bool failed = transition_locked(Event::BackendFailed);
        assert(failed);
    }
}

void DefaultAudioDecoder::notify_backend_failure(AudioDecoderBackendError error) noexcept {
    if (!notifier_) {
        return;
    }

    try {
        (void)notifier_->send(AudioDecoderBackendFailure{
            .error = std::move(error),
        });
    } catch (...) {
        // Runtime failure is still reflected in the worker state if no
        // subscriber is available or a subscriber throws.
    }
}

} // namespace semi::domain
