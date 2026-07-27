#include "domain/worker/demuxer/default_demuxer.hpp"

#include <concepts>
#include <exception>
#include <variant>
#include <utility>

namespace semi::domain {
namespace {

DemuxerError backend_failure(DemuxerBackendError error) {
    DemuxerError result;
    result.code = DemuxerErrorCode::BackendFailure;
    result.message = error.message;
    result.backend_error = std::move(error);
    return result;
}

DemuxerOpenResult select_default_streams(BackendProbeResult probe) {
    DemuxerOpenResult result;
    result.container = std::move(probe.container);

    for (const StreamDescriptor& stream : probe.streams) {
        std::visit(
            [&result, &stream](const auto& config) {
                using Config = std::decay_t<decltype(config)>;
                if constexpr (std::same_as<Config, VideoCodecConfig>) {
                    if (!result.video) {
                        result.video = SelectedStream<VideoCodecConfig>{stream.id, stream.timing, config};
                    }
                } else if constexpr (std::same_as<Config, AudioCodecConfig>) {
                    if (!result.audio) {
                        result.audio = SelectedStream<AudioCodecConfig>{stream.id, stream.timing, config};
                    }
                } else if constexpr (std::same_as<Config, SubtitleCodecConfig>) {
                    if (!result.subtitle) {
                        result.subtitle = SelectedStream<SubtitleCodecConfig>{stream.id, stream.timing, config};
                    }
                }
            },
            stream.config);
    }
    return result;
}

} // namespace

DefaultDemuxer::DefaultDemuxer(std::shared_ptr<DemuxerBackend> backend,
                               std::shared_ptr<AudioPacketSink> audio_packet_sink,
                               std::shared_ptr<infra::Notifier> notifier,
                               std::shared_ptr<Generation> generation)
    : backend_(std::move(backend)),
      audio_packet_sink_(std::move(audio_packet_sink)),
      notifier_(std::move(notifier)),
      generation_(std::move(generation)) {
    if (notifier_) {
        audio_queue_not_full_subscription_ = notifier_->subscribe<AudioQueueNotFull>(
            [this](const AudioQueueNotFull&) {
                queue_not_full_hint_.store(true, std::memory_order_release);
                cv_.notify_one();
            });
    }
}

DefaultDemuxer::~DefaultDemuxer() {
    close();
    audio_queue_not_full_subscription_.reset();
}

std::expected<DemuxerOpenResult, DemuxerError> DefaultDemuxer::open(std::string_view source) {
    std::unique_lock lock(mutex_);
    if (opened_) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "demuxer is already open",
            .backend_error = std::nullopt,
        });
    }
    if (!backend_) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::BackendFailure,
            .message = "demuxer backend is unavailable",
            .backend_error = std::nullopt,
        });
    }
    if (!audio_packet_sink_) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "audio packet sink is unavailable",
            .backend_error = std::nullopt,
        });
    }
    if (!generation_) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "generation is unavailable",
            .backend_error = std::nullopt,
        });
    }

    auto probe = backend_->open(source);
    if (!probe) {
        backend_->close();
        return std::unexpected(backend_failure(std::move(probe.error())));
    }

    auto result = select_default_streams(std::move(*probe));
    generation_->bump();
    if (result.audio) {
        audio_stream_id_ = result.audio->id;
    } else {
        audio_stream_id_.reset();
    }

    opened_ = true;
    state_ = State::Idle;
    stop_requested_ = false;
    queue_not_full_hint_.store(false, std::memory_order_release);
    pending_audio_item_.reset();
    input_end_queued_ = false;
    pending_seek_position_us_.reset();
    return result;
}

std::expected<void, DemuxerError> DefaultDemuxer::start() {
    std::unique_lock lock(mutex_);
    if (!opened_) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "demuxer must be open before starting",
            .backend_error = std::nullopt,
        });
    }

    if (state_ == State::Reading) {
        return {};
    }
    if (state_ == State::Stopping) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "demuxer is stopping",
            .backend_error = std::nullopt,
        });
    }

    stop_requested_ = false;
    state_ = State::Reading;
    if (!worker_running_) {
        if (worker_.joinable()) {
            lock.unlock();
            worker_.join();
            lock.lock();
            if (!opened_) {
                return std::unexpected(DemuxerError{
                    .code = DemuxerErrorCode::InvalidState,
                    .message = "demuxer was closed while restarting its worker",
                    .backend_error = std::nullopt,
                });
            }
        }

        try {
            worker_running_ = true;
            worker_ = std::thread([this] {
                worker_main();
            });
        } catch (...) {
            worker_running_ = false;
            state_ = State::Idle;
            return std::unexpected(DemuxerError{
                .code = DemuxerErrorCode::BackendFailure,
                .message = "failed to start demuxer worker",
                .backend_error = std::nullopt,
            });
        }
    }
    cv_.notify_one();
    return {};
}

void DefaultDemuxer::stop() noexcept {
    std::thread worker;
    {
        std::lock_guard lock(mutex_);
        if (!opened_ || !worker_.joinable()) {
            return;
        }

        stop_requested_ = true;
        state_ = State::Stopping;
        worker = std::move(worker_);
    }

    cv_.notify_one();
    if (worker.joinable()) {
        worker.join();
    }

    std::lock_guard lock(mutex_);
    worker_running_ = false;
    stop_requested_ = false;
    pending_audio_item_.reset();
    queue_not_full_hint_.store(false, std::memory_order_release);
    state_ = State::Stopped;
}

std::expected<void, DemuxerError> DefaultDemuxer::seek(std::int64_t position_us) {
    if (!opened_) {
        return std::unexpected(DemuxerError{
            .code = DemuxerErrorCode::InvalidState,
            .message = "demuxer must be open before seeking",
            .backend_error = std::nullopt,
        });
    }

    pending_seek_position_us_ = position_us;
    return {};
}

void DefaultDemuxer::close() noexcept {
    stop();

    std::lock_guard lock(mutex_);
    if (backend_) {
        backend_->close();
    }
    pending_seek_position_us_.reset();
    audio_stream_id_.reset();
    pending_audio_item_.reset();
    input_end_queued_ = false;
    opened_ = false;
    state_ = State::Constructed;
}

void DefaultDemuxer::worker_main() noexcept {
    try {
        bool running = true;
        while (running) {
            std::optional<AudioPacketQueueItem> pending_audio_item;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] {
                    return stop_requested_ ||
                           (state_ == State::Reading && !input_end_queued_ &&
                            (!pending_audio_item_.has_value() ||
                             queue_not_full_hint_.load(std::memory_order_acquire)));
                });

                if (stop_requested_) {
                    break;
                }

                if (pending_audio_item_) {
                    queue_not_full_hint_.store(false, std::memory_order_release);
                    pending_audio_item = std::move(pending_audio_item_);
                    pending_audio_item_.reset();
                }
            }

            if (pending_audio_item) {
                const bool is_end_of_input =
                    std::holds_alternative<AudioPacketEndOfInput>(*pending_audio_item);
                const auto push_result = audio_packet_sink_->try_push(
                    std::move(*pending_audio_item));
                if (push_result == AudioPacketPushResult::Full) {
                    std::lock_guard lock(mutex_);
                    pending_audio_item_ = std::move(pending_audio_item);
                } else if (is_end_of_input) {
                    std::lock_guard lock(mutex_);
                    input_end_queued_ = true;
                    state_ = State::Idle;
                }
                continue;
            }

            auto read_result = backend_->read_packet();
            {
                std::lock_guard lock(mutex_);
                if (stop_requested_) {
                    break;
                }
            }

            if (!read_result) {
                notify_read_error(std::move(read_result.error()));
                std::lock_guard lock(mutex_);
                state_ = State::Idle;
                continue;
            }

            if (std::holds_alternative<BackendEndOfStream>(*read_result)) {
                if (!audio_stream_id_) {
                    std::lock_guard lock(mutex_);
                    input_end_queued_ = true;
                    state_ = State::Idle;
                    continue;
                }

                AudioPacketQueueItem end_of_input = AudioPacketEndOfInput{
                    .generation = generation_->current(),
                };
                const auto push_result = audio_packet_sink_->try_push(
                    std::move(end_of_input));
                std::lock_guard lock(mutex_);
                if (push_result == AudioPacketPushResult::Full) {
                    pending_audio_item_ = std::move(end_of_input);
                } else {
                    input_end_queued_ = true;
                    state_ = State::Idle;
                }
                continue;
            }

            auto backend_packet = std::get<BackendPacket>(std::move(*read_result));
            if (!audio_stream_id_ || backend_packet.stream_id.value != audio_stream_id_->value) {
                continue;
            }

            AudioPacketQueueItem audio_item{
                std::in_place_type<AudioPacket>,
                std::move(backend_packet.packet),
                generation_->current(),
            };
            const auto push_result = audio_packet_sink_->try_push(std::move(audio_item));
            if (push_result == AudioPacketPushResult::Full) {
                std::lock_guard lock(mutex_);
                pending_audio_item_ = std::move(audio_item);
            }
        }
    } catch (...) {
        notify_read_error(DemuxerBackendError{
            .operation = DemuxerBackendOperation::Read,
            .native_code = 0,
            .message = "demuxer worker failed",
        });
        std::lock_guard lock(mutex_);
        state_ = stop_requested_ ? State::Stopping : State::Idle;
    }

    std::lock_guard lock(mutex_);
    worker_running_ = false;
    if (!stop_requested_ && state_ == State::Reading) {
        state_ = State::Idle;
    }
}

void DefaultDemuxer::notify_read_error(DemuxerBackendError error) noexcept {
    if (!notifier_) {
        return;
    }

    try {
        (void)notifier_->send(DemuxerReadError{.error = std::move(error)});
    } catch (...) {
    }
}

} // namespace semi::domain
