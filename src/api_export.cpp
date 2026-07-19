#include "semi_player/semi_player.h"

#include "application/api_layer.hpp"
#include "infrastructure/log/log.hpp"
#include "ioc/ioc_container.hpp"

#include <memory>

// C ABI 导出层（也是生命周期入口）。
// init     → log::init + IoCContainer::assemble
// shutdown → IoCContainer::dispose + log::shutdown
// 控制命令由 ApiLayer 投递到其私有命令队列（见 docs/modules/api_layer/api_layer.md）。
// 错误约定：docs/error_convention.md。
//
// 导出只由头文件里的 SEMI_API（dllexport）声明负责：编译本目标时
// SEMI_PLAYER_DLL_EXPORT 已定义，故头中声明带 dllexport，定义处无需再标。

namespace {

#define SEMI_LOG_TAG "api"

// 进程默认日志配置。文件写失败时 log 会降级 ConsoleOnly，不阻断播放器 init。
semi::log::Config default_log_config() {
    semi::log::Config config;
    config.file_path = "logs/semi_player.log";
    config.level = semi::log::Level::Info;
    // Info 也进控制台，便于宿主/调试直接看到 assemble 等生命周期日志。
    config.console_level = semi::log::Level::Info;
    return config;
}

// 日志尽量成功；Failed 时仍继续装配（模块日志可走 fallback）。
// AlreadyInitialized：幂等 init 时正常。
bool ensure_log_initialized() noexcept {
    using semi::log::InitResult;
    const InitResult result = semi::log::init(default_log_config());
    switch (result) {
    case InitResult::Ready:
    case InitResult::ConsoleOnly:
    case InitResult::AlreadyInitialized:
        return true;
    case InitResult::Failed:
        return false;
    }
    return false;
}

std::shared_ptr<semi::application::ApiLayer> api_layer() noexcept {
    return semi::ioc::IoCContainer::instance().api_layer();
}

int api_layer_unavailable_status() noexcept {
    return semi::ioc::IoCContainer::instance().is_assembled()
        ? SEMI_ERR_INTERNAL
        : SEMI_ERR_NOT_INITIALIZED;
}

} // namespace

extern "C" {

// ---- Lifecycle ----
int semi_player_init(void) {
    // 先日志，再装配，保证 assemble 的 info 进正式 logger。
    if (!ensure_log_initialized()) {
        // 日志彻底失败仍尝试装配；返回 Internal 让宿主可知日志未就绪。
        if (!semi::ioc::IoCContainer::instance().assemble()) {
            return SEMI_ERR_ASSEMBLE_FAILED;
        }
        return SEMI_ERR_INTERNAL;
    }

    if (!semi::ioc::IoCContainer::instance().assemble()) {
        return SEMI_ERR_ASSEMBLE_FAILED;
    }

    SEMI_LOG_INFO("semi_player_init ok");
    return SEMI_OK;
}

int semi_player_shutdown(void) {
    // 先拆模块（dispose 日志仍可用），再关日志。
    const bool disposed = semi::ioc::IoCContainer::instance().dispose();
    if (!disposed) {
        semi::log::flush();
        semi::log::shutdown();
        return SEMI_ERR_INTERNAL;
    }

    SEMI_LOG_INFO("semi_player_shutdown ok");
    semi::log::flush();
    semi::log::shutdown();
    return SEMI_OK;
}

// ---- Control commands ----
semi_handle_t semi_player_open(const char* src) {
    if (src == nullptr) {
        return 0;
    }
    const auto layer = api_layer();
    return layer ? layer->open(src) : 0;
}

semi_handle_t semi_player_play(void) {
    const auto layer = api_layer();
    return layer ? layer->play() : 0;
}

semi_handle_t semi_player_pause(void) {
    const auto layer = api_layer();
    return layer ? layer->pause() : 0;
}

semi_handle_t semi_player_seek(long long position_us) {
    const auto layer = api_layer();
    return layer ? layer->seek(position_us) : 0;
}

semi_handle_t semi_player_close(void) {
    const auto layer = api_layer();
    return layer ? layer->close() : 0;
}

semi_handle_t semi_player_set_volume(unsigned int volume) {
    const auto layer = api_layer();
    return layer ? layer->set_volume(volume) : 0;
}

// ---- Handle ----
int semi_player_handle_await(semi_handle_t handle, semi_command_result_t* out_result) {
    if (handle == 0 || out_result == nullptr) {
        return handle == 0 ? SEMI_ERR_INVALID_HANDLE : SEMI_ERR_INVALID_ARGUMENT;
    }
    const auto layer = api_layer();
    if (!layer) {
        return api_layer_unavailable_status();
    }

    semi::application::CommandResult result;
    const semi_status_t status = layer->await(handle, result);
    if (status == SEMI_ERR_INVALID_HANDLE) {
        return status;
    }
    out_result->has_media_info = result.has_media_info ? 1U : 0U;
    out_result->media_info.duration_us = result.media_info.duration_us;
    out_result->media_info.video_width = result.media_info.video_width;
    out_result->media_info.video_height = result.media_info.video_height;
    out_result->media_info.has_audio = result.media_info.has_audio ? 1U : 0U;
    out_result->media_info.has_video = result.media_info.has_video ? 1U : 0U;
    out_result->media_info.has_subtitle = result.media_info.has_subtitle ? 1U : 0U;
    return status;
}

int semi_player_handle_cancel(semi_handle_t handle) {
    if (handle == 0) {
        return SEMI_ERR_INVALID_HANDLE;
    }
    const auto layer = api_layer();
    if (!layer) {
        return api_layer_unavailable_status();
    }
    return layer->cancel(handle) ? SEMI_OK : SEMI_ERR_INVALID_STATE;
}

} // extern "C"
