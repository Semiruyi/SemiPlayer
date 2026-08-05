#include "application/api_layer.hpp"

#include "domain/resource/generation/generation.hpp"
#include "domain/worker/audio_decoder/audio_decoder.hpp"
#include "domain/worker/audio_output/audio_output.hpp"
#include "domain/worker/audio_output/audio_output_events.hpp"
#include "domain/worker/audio_resampler/audio_resampler.hpp"
#include "domain/worker/demuxer/demuxer.hpp"
#include "infrastructure/log/log.hpp"
#include "infrastructure/notifier/notifier.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>

#define SEMI_LOG_TAG "api_layer"

namespace semi::application {
namespace {

struct OpenCommand {
    std::string source;
};
struct PlayCommand {};
struct PauseCommand {};
struct SeekCommand {
    std::int64_t position_us;
};
struct CloseCommand {};
struct SetVolumeCommand {
    std::uint32_t volume;
};

using Command = std::variant<OpenCommand, PlayCommand, PauseCommand, SeekCommand, CloseCommand,
                             SetVolumeCommand>;

template <typename... Handlers>
struct Overloaded : Handlers... {
    using Handlers::operator()...;
};

template <typename... Handlers>
Overloaded(Handlers...) -> Overloaded<Handlers...>;

enum class TaskState : std::uint8_t {
    Queued,
    CancelRequested,
    Running,
    Completed,
    Cancelled,
};

bool is_terminal(TaskState state) noexcept {
    return state == TaskState::Completed || state == TaskState::Cancelled;
}

constexpr std::size_t kMaxLiveTasks = 1024;
constexpr std::size_t kMaxPendingEvents = 1024;

} // namespace

struct ApiLayer::Impl {
    Impl(std::shared_ptr<domain::Demuxer> injected_demuxer,
         std::shared_ptr<domain::AudioDecoder> injected_audio_decoder,
         std::shared_ptr<domain::AudioResampler> injected_audio_resampler,
         std::shared_ptr<domain::AudioOutput> injected_audio_output,
         std::shared_ptr<infra::Notifier> injected_notifier,
         std::shared_ptr<domain::Generation> injected_generation)
        : demuxer(std::move(injected_demuxer)),
          audio_decoder(std::move(injected_audio_decoder)),
          audio_resampler(std::move(injected_audio_resampler)),
          audio_output(std::move(injected_audio_output)),
          notifier(std::move(injected_notifier)),
          generation(std::move(injected_generation)) {}

    struct Task {
        Task(CommandHandle task_handle, Command task_command)
            : handle(task_handle), command(std::move(task_command)) {}

        const CommandHandle handle;
        const Command command;
        std::mutex mutex;
        std::condition_variable done_cv;
        TaskState state = TaskState::Queued;
        semi_status_t status = SEMI_ERR_INTERNAL;
        CommandResult result;
        bool consumed = false;
    };

    std::mutex mutex;
    std::condition_variable queue_cv;
    std::unordered_map<CommandHandle, std::shared_ptr<Task>> tasks;
    std::deque<std::shared_ptr<Task>> queue;
    std::deque<CommandHandle> completed_order;
    std::deque<PlayerEvent> pending_events;
    std::thread worker;
    std::shared_ptr<domain::Demuxer> demuxer;
    std::shared_ptr<domain::AudioDecoder> audio_decoder;
    std::shared_ptr<domain::AudioResampler> audio_resampler;
    std::shared_ptr<domain::AudioOutput> audio_output;
    std::shared_ptr<infra::Notifier> notifier;
    std::shared_ptr<domain::Generation> generation;
    std::shared_ptr<infra::Notifier::Subscription> playback_finished_subscription;
    PlayerState player_state = PlayerState::Idle;
    domain::Generation::Value active_generation = 0;
    CommandHandle next_handle = 1;
    bool audio_pipeline_configured = false;
    bool accepting = false;
    bool stopping = false;
};

namespace {

std::size_t discard_completed_until_space(ApiLayer::Impl& impl) {
    std::size_t discarded = 0;
    while (impl.tasks.size() >= kMaxLiveTasks && !impl.completed_order.empty()) {
        const CommandHandle oldest = impl.completed_order.front();
        impl.completed_order.pop_front();
        discarded += impl.tasks.erase(oldest);
    }
    return discarded;
}

struct CommandExecution {
    semi_status_t status = SEMI_ERR_INTERNAL;
    CommandResult result;
    std::optional<PlayerState> next_state;
};

CommandExecution make_failure(semi_status_t status,
                              std::optional<PlayerState> next_state = std::nullopt) {
    CommandExecution execution;
    execution.status = status;
    execution.next_state = next_state;
    return execution;
}

std::optional<PlayerState> idle_state_if_replaced(bool replaced_media) noexcept {
    if (replaced_media) {
        return PlayerState::Idle;
    }
    return std::nullopt;
}

void push_event_locked(ApiLayer::Impl& impl, PlayerEvent event) {
    if (impl.pending_events.size() >= kMaxPendingEvents) {
        impl.pending_events.pop_front();
        SEMI_LOG_WARN("discarded oldest player event to keep event queue capacity {}",
                      kMaxPendingEvents);
    }
    impl.pending_events.push_back(event);
}

bool can_execute(PlayerState state, const Command& command) noexcept {
    return std::visit(
        Overloaded{
            [](const OpenCommand&) { return true; },
            [state](const PlayCommand&) {
                return state != PlayerState::Idle && state != PlayerState::Ended &&
                       state != PlayerState::Error;
            },
            [state](const PauseCommand&) {
                return state != PlayerState::Idle && state != PlayerState::Error;
            },
            [state](const SeekCommand&) {
                return state != PlayerState::Idle && state != PlayerState::Error;
            },
            [](const CloseCommand&) { return true; },
            [](const SetVolumeCommand&) { return true; },
        },
        command);
}

MediaInfo to_media_info(const domain::DemuxerOpenResult& opened) {
    MediaInfo info;
    info.duration_us = opened.container.duration_us.value_or(0);
    info.has_video = opened.video.has_value();
    info.has_audio = opened.audio.has_value();
    info.has_subtitle = opened.subtitle.has_value();
    if (opened.video) {
        info.video_width = opened.video->config.coded_width;
        info.video_height = opened.video->config.coded_height;
    }
    return info;
}

semi_status_t decoder_status(const domain::AudioDecoderError& error) noexcept {
    switch (error.code) {
    case domain::AudioDecoderErrorCode::InvalidState:
        return SEMI_ERR_INVALID_STATE;
    case domain::AudioDecoderErrorCode::BackendFailure:
        return error.backend_error.has_value() ? SEMI_ERR_INVALID_RESOURCE : SEMI_ERR_INTERNAL;
    }
    return SEMI_ERR_INTERNAL;
}

semi_status_t demuxer_status(const domain::DemuxerError& error) noexcept {
    switch (error.code) {
    case domain::DemuxerErrorCode::InvalidState:
        return SEMI_ERR_INVALID_STATE;
    case domain::DemuxerErrorCode::BackendFailure:
        return error.backend_error.has_value() ? SEMI_ERR_INVALID_RESOURCE : SEMI_ERR_INTERNAL;
    }
    return SEMI_ERR_INTERNAL;
}

semi_status_t resampler_status(const domain::AudioResamplerError& error) noexcept {
    switch (error.code) {
    case domain::AudioResamplerErrorCode::InvalidState:
        return SEMI_ERR_INVALID_STATE;
    case domain::AudioResamplerErrorCode::BackendFailure:
        return error.backend_error.has_value() ? SEMI_ERR_INVALID_RESOURCE : SEMI_ERR_INTERNAL;
    }
    return SEMI_ERR_INTERNAL;
}

semi_status_t output_status(const domain::AudioOutputError& error) noexcept {
    switch (error.code) {
    case domain::AudioOutputErrorCode::InvalidState:
        return SEMI_ERR_INVALID_STATE;
    case domain::AudioOutputErrorCode::BackendFailure:
        return error.backend_error.has_value() ? SEMI_ERR_INVALID_RESOURCE : SEMI_ERR_INTERNAL;
    }
    return SEMI_ERR_INTERNAL;
}

void close_pipeline(ApiLayer::Impl& impl) noexcept {
    if (impl.demuxer) {
        impl.demuxer->close();
    }
    if (impl.audio_decoder) {
        impl.audio_decoder->unconfigure();
    }
    if (impl.audio_resampler) {
        impl.audio_resampler->unconfigure();
    }
    if (impl.audio_output) {
        impl.audio_output->unconfigure();
    }
    impl.audio_pipeline_configured = false;
    {
        std::lock_guard lock(impl.mutex);
        impl.active_generation = 0;
    }
}

void handle_playback_finished(ApiLayer::Impl& impl,
                              const domain::AudioPlaybackFinished& event) noexcept {
    std::lock_guard lock(impl.mutex);
    if (event.generation != impl.active_generation) {
        return;
    }
    if (impl.player_state == PlayerState::Ready ||
        impl.player_state == PlayerState::Playing ||
        impl.player_state == PlayerState::Paused) {
        impl.player_state = PlayerState::Ended;
        push_event_locked(impl, PlayerEvent{.type = PlayerEventType::PlaybackFinished});
    }
}

std::expected<domain::DemuxerOpenResult, CommandExecution>
open_demuxer(ApiLayer::Impl& impl, const std::string& source, bool replaced_media) {
    try {
        auto opened = impl.demuxer->open(source);
        if (!opened) {
            SEMI_LOG_ERROR("demuxer open failed: {}", opened.error().message);
            return std::unexpected(make_failure(demuxer_status(opened.error()),
                                                idle_state_if_replaced(replaced_media)));
        }
        return *std::move(opened);
    } catch (const std::exception& error) {
        SEMI_LOG_ERROR("demuxer open threw an exception: {}", error.what());
    } catch (...) {
        SEMI_LOG_ERROR("demuxer open threw an unknown exception");
    }
    return std::unexpected(make_failure(SEMI_ERR_INTERNAL,
                                        idle_state_if_replaced(replaced_media)));
}

std::expected<void, CommandExecution>
configure_audio_pipeline(ApiLayer::Impl& impl, const domain::DemuxerOpenResult& opened) {
    if (!opened.audio.has_value()) {
        return {};
    }

    if (!impl.audio_decoder || !impl.audio_resampler || !impl.audio_output) {
        SEMI_LOG_ERROR("audio stream is present but audio pipeline is not assembled");
        close_pipeline(impl);
        return std::unexpected(make_failure(SEMI_ERR_INTERNAL, PlayerState::Idle));
    }

    auto decoded = impl.audio_decoder->configure(opened.audio->config);
    if (!decoded) {
        SEMI_LOG_ERROR("audio decoder configure failed: {}", decoded.error().message);
        close_pipeline(impl);
        return std::unexpected(make_failure(decoder_status(decoded.error()), PlayerState::Idle));
    }

    auto output = impl.audio_output->configure({});
    if (!output) {
        SEMI_LOG_ERROR("audio output configure failed: {}", output.error().message);
        close_pipeline(impl);
        return std::unexpected(make_failure(output_status(output.error()), PlayerState::Idle));
    }

    auto resampled = impl.audio_resampler->configure(decoded->decoded_format,
                                                     output->playback_format);
    if (!resampled) {
        SEMI_LOG_ERROR("audio resampler configure failed: {}", resampled.error().message);
        close_pipeline(impl);
        return std::unexpected(make_failure(resampler_status(resampled.error()), PlayerState::Idle));
    }

    impl.audio_pipeline_configured = true;
    return {};
}

CommandExecution make_open_success(const domain::DemuxerOpenResult& opened) {
    CommandExecution execution;
    execution.status = SEMI_OK;
    execution.next_state = PlayerState::Ready;
    execution.result.has_media_info = true;
    execution.result.media_info = to_media_info(opened);
    const MediaInfo& media_info = execution.result.media_info;
    SEMI_LOG_INFO("media opened: duration_us={}, video={}x{}, audio={}, subtitle={}",
                  media_info.duration_us,
                  media_info.video_width,
                  media_info.video_height,
                  media_info.has_audio,
                  media_info.has_subtitle);
    return execution;
}

CommandExecution execute_open(const OpenCommand& command,
                              PlayerState current_state,
                              ApiLayer::Impl& impl) {
    if (command.source.empty()) {
        return make_failure(SEMI_ERR_INVALID_ARGUMENT);
    }

    const bool replaced_media = current_state != PlayerState::Idle;
    if (replaced_media) {
        close_pipeline(impl);
    }

    auto opened = open_demuxer(impl, command.source, replaced_media);
    if (!opened) {
        return opened.error();
    }

    auto audio_configured = configure_audio_pipeline(impl, *opened);
    if (!audio_configured) {
        return audio_configured.error();
    }

    if (impl.generation) {
        std::lock_guard lock(impl.mutex);
        impl.active_generation = impl.generation->current();
    }
    return make_open_success(*opened);
}

CommandExecution execute_play(PlayerState current_state, ApiLayer::Impl& impl) {
    if (current_state == PlayerState::Playing) {
        return make_failure(SEMI_OK);
    }

    if (impl.audio_pipeline_configured && impl.audio_output) {
        auto started = impl.audio_output->start_playback();
        if (!started) {
            SEMI_LOG_ERROR("audio output start playback failed: {}", started.error().message);
            return make_failure(output_status(started.error()));
        }
    }

    CommandExecution execution;
    execution.status = SEMI_OK;
    execution.next_state = PlayerState::Playing;
    return execution;
}

CommandExecution execute_pause(PlayerState current_state, ApiLayer::Impl& impl) {
    if (current_state != PlayerState::Playing) {
        return make_failure(SEMI_OK);
    }

    if (impl.audio_pipeline_configured && impl.audio_output) {
        auto paused = impl.audio_output->pause_playback();
        if (!paused) {
            SEMI_LOG_ERROR("audio output pause playback failed: {}", paused.error().message);
            return make_failure(output_status(paused.error()));
        }
    }

    CommandExecution execution;
    execution.status = SEMI_OK;
    execution.next_state = PlayerState::Paused;
    return execution;
}

CommandExecution execute_seek(std::int64_t position_us,
                               PlayerState current_state,
                               ApiLayer::Impl& impl) noexcept {
    if (position_us < 0 || !impl.demuxer) {
        return make_failure(position_us < 0 ? SEMI_ERR_INVALID_ARGUMENT : SEMI_ERR_INTERNAL);
    }
    try {
        auto result = impl.demuxer->seek(position_us);
        if (!result) {
            return make_failure(demuxer_status(result.error()));
        }
    } catch (...) {
        SEMI_LOG_ERROR("demuxer seek failed with an exception");
        return make_failure(SEMI_ERR_INTERNAL);
    }

    if (impl.generation) {
        std::lock_guard lock(impl.mutex);
        impl.active_generation = impl.generation->current();
    }

    CommandExecution execution;
    execution.status = SEMI_OK;
    execution.next_state = current_state == PlayerState::Ended ? PlayerState::Paused : current_state;
    return execution;
}

CommandExecution execute_close(PlayerState current_state, ApiLayer::Impl& impl) noexcept {
    CommandExecution execution;
    execution.status = SEMI_OK;
    execution.next_state = PlayerState::Idle;
    if (current_state != PlayerState::Idle) {
        close_pipeline(impl);
        SEMI_LOG_INFO("media closed");
    }
    return execution;
}

CommandExecution execute_command(PlayerState current_state,
                                 const Command& command,
                                 ApiLayer::Impl& impl) noexcept {
    try {
        if (!can_execute(current_state, command)) {
            CommandExecution execution;
            execution.status = SEMI_ERR_INVALID_STATE;
            return execution;
        }

        return std::visit(
            Overloaded{
                [&impl, current_state](const OpenCommand& value) {
                    return execute_open(value, current_state, impl);
                },
                [&impl, current_state](const PlayCommand&) {
                    return execute_play(current_state, impl);
                },
                [&impl, current_state](const PauseCommand&) {
                    return execute_pause(current_state, impl);
                },
                [&impl, current_state](const SeekCommand& value) {
                    return execute_seek(value.position_us, current_state, impl);
                },
                [&impl, current_state](const CloseCommand&) {
                    return execute_close(current_state, impl);
                },
                [](const SetVolumeCommand&) -> CommandExecution { return {}; },
            },
            command);
    } catch (const std::exception& error) {
        SEMI_LOG_ERROR("command execution failed: {}", error.what());
        return {};
    } catch (...) {
        SEMI_LOG_ERROR("command execution failed with an unknown exception");
        return {};
    }
}

void complete_task(ApiLayer::Impl& impl,
                   const std::shared_ptr<ApiLayer::Impl::Task>& task,
                   semi_status_t status,
                   CommandResult result) {
    {
        std::lock_guard lock(task->mutex);
        task->status = status;
        task->result = std::move(result);
        task->state = status == SEMI_ERR_CANCELLED ? TaskState::Cancelled : TaskState::Completed;
    }
    task->done_cv.notify_all();

    std::lock_guard lock(impl.mutex);
    impl.completed_order.push_back(task->handle);
}

void worker_main(ApiLayer::Impl& impl) {
    for (;;) {
        std::shared_ptr<ApiLayer::Impl::Task> task;
        bool stopping = false;
        {
            std::unique_lock lock(impl.mutex);
            impl.queue_cv.wait(lock, [&impl] {
                return impl.stopping || !impl.queue.empty();
            });
            if (impl.queue.empty()) {
                if (impl.stopping) {
                    return;
                }
                continue;
            }
            task = std::move(impl.queue.front());
            impl.queue.pop_front();
            stopping = impl.stopping;
        }

        bool cancelled = stopping;
        {
            std::lock_guard lock(task->mutex);
            if (task->state == TaskState::CancelRequested) {
                cancelled = true;
            } else if (!cancelled) {
                task->state = TaskState::Running;
            }
        }

        if (cancelled) {
            complete_task(impl, task, SEMI_ERR_CANCELLED, {});
            continue;
        }

        if (!impl.demuxer) {
            complete_task(impl, task, SEMI_ERR_INTERNAL, {});
            continue;
        }
        PlayerState current_state = PlayerState::Idle;
        {
            std::lock_guard lock(impl.mutex);
            current_state = impl.player_state;
        }

        CommandExecution execution = execute_command(current_state, task->command, impl);
        if (execution.next_state.has_value()) {
            std::lock_guard lock(impl.mutex);
            impl.player_state = *execution.next_state;
        }
        complete_task(impl, task, execution.status, std::move(execution.result));
    }
}

} // namespace

ApiLayer::ApiLayer(std::shared_ptr<domain::Demuxer> demuxer,
                   std::shared_ptr<domain::AudioDecoder> audio_decoder,
                   std::shared_ptr<domain::AudioResampler> audio_resampler,
                   std::shared_ptr<domain::AudioOutput> audio_output,
                   std::shared_ptr<infra::Notifier> notifier,
                   std::shared_ptr<domain::Generation> generation)
    : impl_(std::make_unique<Impl>(std::move(demuxer),
                                   std::move(audio_decoder),
                                   std::move(audio_resampler),
                                   std::move(audio_output),
                                   std::move(notifier),
                                   std::move(generation))) {
    if (impl_->notifier) {
        impl_->playback_finished_subscription =
            impl_->notifier->subscribe<domain::AudioPlaybackFinished>(
                [impl = impl_.get()](const domain::AudioPlaybackFinished& event) {
                    handle_playback_finished(*impl, event);
                });
    }
}

ApiLayer::~ApiLayer() {
    (void)stop();
}

bool ApiLayer::start() noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        if (impl_->accepting) {
            return true;
        }
        impl_->stopping = false;
        impl_->worker = std::thread([impl = impl_.get()] {
            worker_main(*impl);
        });
        impl_->accepting = true;
        SEMI_LOG_INFO("command worker started");
        return true;
    } catch (...) {
        SEMI_LOG_ERROR("failed to start command worker");
        return false;
    }
}

bool ApiLayer::stop() noexcept {
    std::thread worker;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->worker.joinable()) {
            impl_->accepting = false;
            impl_->stopping = true;
            return true;
        }
        impl_->accepting = false;
        impl_->stopping = true;
        worker = std::move(impl_->worker);
    }
    impl_->queue_cv.notify_all();
    worker.join();
    SEMI_LOG_INFO("command worker stopped");
    return true;
}

namespace {

template <typename CommandType>
CommandHandle enqueue(ApiLayer::Impl& impl, CommandType command) {
    try {
        std::shared_ptr<ApiLayer::Impl::Task> task;
        std::size_t discarded = 0;
        bool capacity_rejected = false;
        {
            std::lock_guard lock(impl.mutex);
            if (!impl.accepting) {
                return 0;
            }
            discarded = discard_completed_until_space(impl);
            if (impl.tasks.size() >= kMaxLiveTasks) {
                capacity_rejected = true;
            } else {
                while (impl.next_handle == 0 || impl.tasks.contains(impl.next_handle)) {
                    ++impl.next_handle;
                }
                const CommandHandle handle = impl.next_handle++;
                task = std::make_shared<ApiLayer::Impl::Task>(handle, Command{std::move(command)});
                impl.tasks.emplace(handle, task);
                impl.queue.push_back(task);
            }
        }
        if (discarded != 0) {
            SEMI_LOG_WARN("discarded {} completed command result(s) to free task capacity", discarded);
        }
        if (capacity_rejected) {
            SEMI_LOG_WARN("command rejected: task capacity {} is occupied by queued or running commands",
                          kMaxLiveTasks);
            return 0;
        }
        impl.queue_cv.notify_one();
        return task->handle;
    } catch (...) {
        SEMI_LOG_ERROR("failed to enqueue command");
        return 0;
    }
}

} // namespace

CommandHandle ApiLayer::enqueue_open(std::string source) {
    return enqueue(*impl_, OpenCommand{std::move(source)});
}

CommandHandle ApiLayer::enqueue_play() {
    return enqueue(*impl_, PlayCommand{});
}

CommandHandle ApiLayer::enqueue_pause() {
    return enqueue(*impl_, PauseCommand{});
}

CommandHandle ApiLayer::enqueue_seek(std::int64_t position_us) {
    return enqueue(*impl_, SeekCommand{position_us});
}

CommandHandle ApiLayer::enqueue_close() {
    return enqueue(*impl_, CloseCommand{});
}

CommandHandle ApiLayer::enqueue_set_volume(std::uint32_t volume) {
    return enqueue(*impl_, SetVolumeCommand{volume});
}

CommandHandle ApiLayer::open(std::string source) {
    return enqueue_open(std::move(source));
}

CommandHandle ApiLayer::play() {
    return enqueue_play();
}

CommandHandle ApiLayer::pause() {
    return enqueue_pause();
}

CommandHandle ApiLayer::seek(std::int64_t position_us) {
    return enqueue_seek(position_us);
}

CommandHandle ApiLayer::close() {
    return enqueue_close();
}

CommandHandle ApiLayer::set_volume(std::uint32_t volume) {
    return enqueue_set_volume(volume);
}

semi_status_t ApiLayer::await(CommandHandle handle, CommandResult& out_result) {
    std::shared_ptr<Impl::Task> task;
    {
        std::lock_guard lock(impl_->mutex);
        const auto it = impl_->tasks.find(handle);
        if (it == impl_->tasks.end()) {
            return SEMI_ERR_INVALID_HANDLE;
        }
        task = it->second;
    }

    semi_status_t status = SEMI_ERR_INTERNAL;
    {
        std::unique_lock lock(task->mutex);
        task->done_cv.wait(lock, [&task] {
            return is_terminal(task->state);
        });
        if (task->consumed) {
            return SEMI_ERR_INVALID_HANDLE;
        }
        task->consumed = true;
        status = task->status;
        out_result = task->result;
    }

    {
        std::lock_guard lock(impl_->mutex);
        const auto it = impl_->tasks.find(handle);
        if (it != impl_->tasks.end() && it->second == task) {
            impl_->tasks.erase(it);
        }
    }
    return status;
}

semi_status_t ApiLayer::poll_event(PlayerEvent& out_event) noexcept {
    std::lock_guard lock(impl_->mutex);
    if (impl_->pending_events.empty()) {
        out_event = PlayerEvent{};
        return SEMI_OK;
    }

    out_event = impl_->pending_events.front();
    impl_->pending_events.pop_front();
    return SEMI_OK;
}

bool ApiLayer::cancel(CommandHandle handle) noexcept {
    std::shared_ptr<Impl::Task> task;
    {
        std::lock_guard lock(impl_->mutex);
        const auto it = impl_->tasks.find(handle);
        if (it == impl_->tasks.end()) {
            return false;
        }
        task = it->second;
    }

    std::lock_guard lock(task->mutex);
    if (task->state != TaskState::Queued) {
        return false;
    }
    task->state = TaskState::CancelRequested;
    return true;
}

} // namespace semi::application
