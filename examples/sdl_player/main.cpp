#include "video_presenter.hpp"

#include "semi_player/semi_player.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace {

const char* status_name(int status) noexcept {
    switch (status) {
    case SEMI_OK:
        return "SEMI_OK";
    case SEMI_ERR_NOT_INITIALIZED:
        return "SEMI_ERR_NOT_INITIALIZED";
    case SEMI_ERR_INVALID_STATE:
        return "SEMI_ERR_INVALID_STATE";
    case SEMI_ERR_CANCELLED:
        return "SEMI_ERR_CANCELLED";
    case SEMI_ERR_ASSEMBLE_FAILED:
        return "SEMI_ERR_ASSEMBLE_FAILED";
    case SEMI_ERR_INTERNAL:
        return "SEMI_ERR_INTERNAL";
    case SEMI_ERR_INVALID_ARGUMENT:
        return "SEMI_ERR_INVALID_ARGUMENT";
    case SEMI_ERR_INVALID_HANDLE:
        return "SEMI_ERR_INVALID_HANDLE";
    case SEMI_ERR_INVALID_RESOURCE:
        return "SEMI_ERR_INVALID_RESOURCE";
    default:
        return "SEMI_STATUS_UNKNOWN";
    }
}

bool await_command(std::string_view name,
                   semi_handle_t handle,
                   semi_command_result_t* result = nullptr) noexcept {
    if (handle == 0) {
        std::fprintf(stderr, "[sdl host] %.*s returned no handle\n",
                     static_cast<int>(name.size()), name.data());
        return false;
    }

    semi_command_result_t local_result{};
    const int status = semi_player_handle_await(
        handle, result != nullptr ? result : &local_result);
    if (status != SEMI_OK) {
        std::fprintf(stderr, "[sdl host] %.*s failed: %s (%d)\n",
                     static_cast<int>(name.size()), name.data(),
                     status_name(status), status);
        return false;
    }
    return true;
}

class CommandAwaiter final {
public:
    CommandAwaiter() : worker_([this] { run(); }) {}

    ~CommandAwaiter() {
        shutdown();
    }

    CommandAwaiter(const CommandAwaiter&) = delete;
    CommandAwaiter& operator=(const CommandAwaiter&) = delete;

    bool submit(semi_handle_t handle, const char* name) noexcept {
        if (handle == 0) {
            std::fprintf(stderr, "[sdl host] %s returned no handle\n", name);
            return false;
        }

        try {
            {
                std::scoped_lock lock(mutex_);
                if (!accepting_) {
                    return false;
                }
                commands_.push_back(Command{.handle = handle, .name = name});
            }
            cv_.notify_one();
            return true;
        } catch (...) {
            std::fprintf(stderr,
                         "[sdl host] could not queue %s completion; waiting inline\n",
                         name);
            return await_command(name, handle);
        }
    }

    void shutdown() noexcept {
        {
            std::scoped_lock lock(mutex_);
            accepting_ = false;
        }
        cv_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    struct Command {
        semi_handle_t handle = 0;
        const char* name = nullptr;
    };

    void run() noexcept {
        for (;;) {
            Command command;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return !commands_.empty() || !accepting_; });
                if (commands_.empty()) {
                    return;
                }
                command = commands_.front();
                commands_.pop_front();
            }
            await_command(command.name, command.handle);
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Command> commands_;
    bool accepting_ = true;
    std::thread worker_;
};

class SdlPlayerApplication final {
public:
    ~SdlPlayerApplication() {
        cleanup();
    }

    [[nodiscard]] bool initialize(const char* media_path) {
        const int init_status = semi_player_init();
        if (init_status != SEMI_OK) {
            std::fprintf(stderr, "[sdl host] player init failed: %s (%d)\n",
                         status_name(init_status), init_status);
            return false;
        }
        player_initialized_ = true;

        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::fprintf(stderr, "[sdl host] SDL init failed: %s\n", SDL_GetError());
            return false;
        }
        sdl_initialized_ = true;

        frame_event_ = SDL_RegisterEvents(1);
        if (frame_event_ == 0) {
            std::fprintf(stderr, "[sdl host] SDL event registration failed: %s\n",
                         SDL_GetError());
            return false;
        }
        mailbox_.set_wake_event(frame_event_);

        semi_video_output_config_t video_config{};
        video_config.struct_size = sizeof(video_config);
        video_config.pixel_format = SEMI_VIDEO_PIXEL_FORMAT_RGBA8888;
        video_config.on_frame = &SdlPlayerApplication::on_video_frame;
        video_config.user_data = &mailbox_;
        if (!await_command("configure video",
                           semi_player_configure_video_output(&video_config))) {
            return false;
        }

        semi_command_result_t open_result{};
        if (!await_command("open", semi_player_open(media_path), &open_result)) {
            return false;
        }
        media_open_ = true;
        if (!open_result.has_media_info || !open_result.media_info.has_video) {
            std::fprintf(stderr, "[sdl host] media has no video stream\n");
            return false;
        }
        duration_us_ = std::max<std::int64_t>(0, open_result.media_info.duration_us);

        int window_width = 1920;
        int window_height = 1080;
        fit_initial_window(open_result.media_info.video_width,
                           open_result.media_info.video_height,
                           window_width,
                           window_height);
        window_ = SDL_CreateWindow("SemiPlayer SDL Host",
                                   window_width,
                                   window_height,
                                   SDL_WINDOW_RESIZABLE |
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (window_ == nullptr) {
            std::fprintf(stderr, "[sdl host] create window failed: %s\n", SDL_GetError());
            return false;
        }

        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (renderer_ == nullptr) {
            std::fprintf(stderr, "[sdl host] create renderer failed: %s\n", SDL_GetError());
            return false;
        }
        if (!SDL_SetRenderVSync(renderer_, 1)) {
            std::fprintf(stderr, "[sdl host] vsync unavailable: %s\n", SDL_GetError());
        }
        presenter_ = std::make_unique<semi::example::VideoPresenter>(renderer_);

        if (!await_command("play", semi_player_play())) {
            return false;
        }
        play_requested_ = true;

        std::printf("[sdl host] controls: Space play/pause, Left/Right seek 5s, "
                    "F11 fullscreen, Esc quit\n");
        return true;
    }

    int run() noexcept {
        bool running = true;
        while (running) {
            SDL_Event event{};
            if (SDL_WaitEventTimeout(&event, 16)) {
                running = handle_event(event);
                while (running && SDL_PollEvent(&event)) {
                    running = handle_event(event);
                }
            }
            if (!running) {
                return fatal_error_ ? 1 : 0;
            }

            if (!poll_player_events()) {
                return 1;
            }
            if (!presenter_->present_latest(mailbox_)) {
                return 1;
            }
            if (playback_finished_) {
                return 0;
            }
        }
        return 0;
    }

private:
    static void on_video_frame(void* user_data,
                               const semi_video_frame_t* frame) noexcept {
        if (user_data != nullptr) {
            static_cast<semi::example::LatestFrameMailbox*>(user_data)->publish(frame);
        }
    }

    static void fit_initial_window(unsigned int video_width,
                                   unsigned int video_height,
                                   int& window_width,
                                   int& window_height) noexcept {
        if (video_width == 0 || video_height == 0) {
            return;
        }
        constexpr double max_width = 1280.0;
        constexpr double max_height = 720.0;
        const double scale = std::min(
            {1.0, max_width / video_width, max_height / video_height});
        window_width = std::max(320, static_cast<int>(video_width * scale));
        window_height = std::max(180, static_cast<int>(video_height * scale));
    }

    bool handle_event(const SDL_Event& event) noexcept {
        if (event.type == SDL_EVENT_QUIT) {
            return false;
        }
        if (event.type == frame_event_) {
            return true;
        }
        if (event.type == SDL_EVENT_WINDOW_EXPOSED ||
            event.type == SDL_EVENT_WINDOW_RESIZED ||
            event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            if (!presenter_->redraw()) {
                fatal_error_ = true;
                return false;
            }
            return true;
        }
        if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
            return true;
        }

        switch (event.key.key) {
        case SDLK_ESCAPE:
            return false;
        case SDLK_SPACE:
            toggle_playback();
            break;
        case SDLK_LEFT:
            seek_relative(-5'000'000);
            break;
        case SDLK_RIGHT:
            seek_relative(5'000'000);
            break;
        case SDLK_F11:
            fullscreen_ = !fullscreen_;
            if (!SDL_SetWindowFullscreen(window_, fullscreen_)) {
                std::fprintf(stderr, "[sdl host] fullscreen failed: %s\n",
                             SDL_GetError());
                fullscreen_ = !fullscreen_;
            }
            break;
        default:
            break;
        }
        return true;
    }

    void toggle_playback() noexcept {
        const semi_handle_t handle = play_requested_
            ? semi_player_pause()
            : semi_player_play();
        if (command_awaiter_.submit(handle, play_requested_ ? "pause" : "play")) {
            play_requested_ = !play_requested_;
            SDL_SetWindowTitle(window_, play_requested_
                ? "SemiPlayer SDL Host"
                : "SemiPlayer SDL Host (paused)");
        }
    }

    void seek_relative(std::int64_t delta_us) noexcept {
        const std::int64_t current = presenter_->current_pts_us();
        const std::int64_t target = std::clamp(
            current + delta_us, std::int64_t{0}, duration_us_);
        command_awaiter_.submit(semi_player_seek(target), "seek");
    }

    bool poll_player_events() noexcept {
        for (;;) {
            semi_player_event_t event{};
            const int status = semi_player_poll_event(&event);
            if (status != SEMI_OK) {
                std::fprintf(stderr, "[sdl host] poll event failed: %s (%d)\n",
                             status_name(status), status);
                return false;
            }
            if (event.type == SEMI_PLAYER_EVENT_NONE) {
                return true;
            }
            if (event.type == SEMI_PLAYER_EVENT_PLAYBACK_FINISHED) {
                play_requested_ = false;
                playback_finished_ = true;
            }
        }
    }

    void cleanup() noexcept {
        command_awaiter_.shutdown();
        if (media_open_) {
            await_command("close", semi_player_close());
            media_open_ = false;
        }
        presenter_.reset();
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        if (sdl_initialized_) {
            SDL_Quit();
            sdl_initialized_ = false;
        }
        if (player_initialized_) {
            const int status = semi_player_shutdown();
            if (status != SEMI_OK) {
                std::fprintf(stderr, "[sdl host] shutdown failed: %s (%d)\n",
                             status_name(status), status);
            }
            player_initialized_ = false;
        }
    }

    semi::example::LatestFrameMailbox mailbox_;
    CommandAwaiter command_awaiter_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::unique_ptr<semi::example::VideoPresenter> presenter_;
    Uint32 frame_event_ = 0;
    std::int64_t duration_us_ = 0;
    bool player_initialized_ = false;
    bool sdl_initialized_ = false;
    bool media_open_ = false;
    bool play_requested_ = false;
    bool playback_finished_ = false;
    bool fullscreen_ = false;
    bool fatal_error_ = false;
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <media-file>\n", argc > 0 ? argv[0] : "semi_player_sdl");
        return 2;
    }

    try {
        SdlPlayerApplication application;
        if (!application.initialize(argv[1])) {
            return 1;
        }
        return application.run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[sdl host] fatal error: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "[sdl host] fatal unknown error\n");
        return 1;
    }
}
