#include "application/api_layer.hpp"

#include "domain/demuxer/demuxer.hpp"
#include "infrastructure/log/log.hpp"

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

} // namespace

struct ApiLayer::Impl {
    explicit Impl(std::shared_ptr<domain::Demuxer> injected_demuxer)
        : demuxer(std::move(injected_demuxer)) {}

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
    std::thread worker;
    std::shared_ptr<domain::Demuxer> demuxer;
    CommandHandle next_handle = 1;
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

[[maybe_unused]] semi_status_t execute_command(const Command&) noexcept {
    // 媒体业务模块尚未接入。任务基础设施仍须由命令线程完成并通知 await。
    return SEMI_ERR_INTERNAL;
}

struct CommandExecution {
    semi_status_t status = SEMI_ERR_INTERNAL;
    CommandResult result;
};

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

CommandExecution execute_open(const OpenCommand& command, domain::Demuxer& demuxer) {
    auto opened = demuxer.open(command.source);
    if (!opened) {
        SEMI_LOG_ERROR("demuxer open failed: {}", opened.error().message);
        CommandExecution execution;
        switch (opened.error().code) {
        case domain::DemuxerErrorCode::InvalidState:
            execution.status = SEMI_ERR_INVALID_STATE;
            break;
        case domain::DemuxerErrorCode::BackendFailure:
            execution.status = opened.error().backend_error.has_value()
                ? SEMI_ERR_INVALID_RESOURCE
                : SEMI_ERR_INTERNAL;
            break;
        }
        return execution;
    }

    CommandExecution execution;
    execution.status = SEMI_OK;
    execution.result.has_media_info = true;
    execution.result.media_info = to_media_info(*opened);
    const MediaInfo& media_info = execution.result.media_info;
    SEMI_LOG_INFO("media opened: duration_us={}, video={}x{}, audio={}, subtitle={}",
                  media_info.duration_us,
                  media_info.video_width,
                  media_info.video_height,
                  media_info.has_audio,
                  media_info.has_subtitle);
    return execution;
}

CommandExecution execute_command(const Command& command, domain::Demuxer& demuxer) noexcept {
    try {
        return std::visit(
            Overloaded{
                [&demuxer](const OpenCommand& value) {
                    return execute_open(value, demuxer);
                },
                [](const PlayCommand&) -> CommandExecution { return {}; },
                [](const PauseCommand&) -> CommandExecution { return {}; },
                [](const SeekCommand&) -> CommandExecution { return {}; },
                [](const CloseCommand&) -> CommandExecution { return {}; },
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
        CommandExecution execution = execute_command(task->command, *impl.demuxer);
        complete_task(impl, task, execution.status, std::move(execution.result));
    }
}

} // namespace

ApiLayer::ApiLayer(std::shared_ptr<domain::Demuxer> demuxer)
    : impl_(std::make_unique<Impl>(std::move(demuxer))) {}

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
