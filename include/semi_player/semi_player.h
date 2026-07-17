#ifndef SEMI_PLAYER_H
#define SEMI_PLAYER_H

/* SemiPlayer C ABI — consumed by Flutter(Dart) via dart:ffi.
 * 对应 docs/modules/api_layer/api_layer.md 的三类接口：
 *   控制命令（投递命令、返回句柄，可 await/cancel）
 *   状态查询（同步直接返回快照，不走命令队列）
 *   事件流（progress，Stream）
 * 命令句柄的 await 结果 / 取消信号在 Dart 侧的具体绑定形态见 api_layer.md 待确认项。
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
SEMI_API int           semi_player_set_volume(unsigned int volume);

/* ---- Queries: synchronous snapshot, do NOT go through the command queue ---- */
SEMI_API int       semi_player_get_state(int *out_state);
SEMI_API long long semi_player_get_duration(void);

/* ---- Handle: cancel an in-flight command ---- */
SEMI_API int semi_player_handle_cancel(semi_handle_t handle);

/* ---- Progress stream ---- */
typedef void (*semi_progress_cb)(long long position_us, void *user_data);
SEMI_API int semi_player_progress_subscribe(semi_progress_cb cb, void *user_data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SEMI_PLAYER_H */
