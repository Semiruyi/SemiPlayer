#ifndef SEMI_PLAYER_H
#define SEMI_PLAYER_H

/* SemiPlayer C ABI
 * 对应 docs/modules/api_layer/api_layer.md 的三类接口：
 *   控制命令（投递命令、返回句柄，可 await/cancel）
 * 同步返回的 int 状态码见 status.h / docs/error_convention.md。
 */

#include "semi_player/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(WIN32) || defined(_WIN32)
  #ifdef SEMI_PLAYER_DLL_EXPORT
    #define SEMI_API __declspec(dllexport)
  #else
    #define SEMI_API __declspec(dllimport)
  #endif
#else
  #define SEMI_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

/* Opaque handle to an in-flight command. 0 == invalid/no handle. */
typedef unsigned long long semi_handle_t;

typedef struct semi_media_info {
    long long duration_us;
    unsigned int video_width;
    unsigned int video_height;
    unsigned char has_audio;
    unsigned char has_video;
    unsigned char has_subtitle;
} semi_media_info_t;

/* await 接受有效 handle 时写入。has_media_info 仅在 open 成功时为非零。 */
typedef struct semi_command_result {
    semi_media_info_t media_info;
    unsigned char has_media_info;
} semi_command_result_t;

typedef enum semi_player_event_type {
    SEMI_PLAYER_EVENT_NONE = 0,
    SEMI_PLAYER_EVENT_PLAYBACK_FINISHED = 1
} semi_player_event_type_t;

typedef struct semi_player_event {
    semi_player_event_type_t type;
} semi_player_event_t;

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Lifecycle (Player layer; see docs/lifecycle.md) ---- */
/* 返回 semi_status_t：SEMI_OK 或 SEMI_ERR_* */
SEMI_API int semi_player_init(void);
SEMI_API int semi_player_shutdown(void);

/* ---- Control commands: post to command queue, return handle immediately ---- */
SEMI_API semi_handle_t semi_player_open(const char *src);
SEMI_API semi_handle_t semi_player_play(void);
SEMI_API semi_handle_t semi_player_pause(void);
SEMI_API semi_handle_t semi_player_seek(long long position_us);
SEMI_API semi_handle_t semi_player_close(void);
SEMI_API semi_handle_t semi_player_set_volume(unsigned int volume);

/* ---- Events ---- */
/* Non-blocking. Returns SEMI_OK when out_event is written; no pending event is
 * represented as SEMI_PLAYER_EVENT_NONE rather than an error. */
SEMI_API int semi_player_poll_event(semi_player_event_t *out_event);

/* ---- Handle ---- */
/* 阻塞到命令结束，写入结果并消费 handle；返回该命令的最终 semi_status。
 * open 的资源无法打开或探测时返回 SEMI_ERR_INVALID_RESOURCE。 */
SEMI_API int semi_player_handle_await(semi_handle_t handle, semi_command_result_t *out_result);
/* 仅接受尚未开始执行的取消请求；任务仍由命令线程完成并通知 await。 */
SEMI_API int semi_player_handle_cancel(semi_handle_t handle);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SEMI_PLAYER_H */
