#include "semi_player/semi_player.h"

#include "player.hpp"

// C ABI 导出层。每个函数转投到 C++ 引擎（ApiLayer/Player）。
// 控制命令当前为桩：返回 0/invalid handle，待 ApiLayer 命令队列接入后实现。
// 对应接口分类见 docs/modules/api_layer/api_layer.md。
//
// 导出只由头文件里的 SEMI_API（dllexport）声明负责：编译本目标时
// SEMI_PLAYER_DLL_EXPORT 已定义，故头中声明带 dllexport，定义处无需再标。

extern "C" {

// ---- Lifecycle ----
int semi_player_init(void) {
    return semi::player_init();
}

int semi_player_shutdown(void) {
    return semi::player_shutdown();
}

// ---- Control commands (TODO: post to ApiLayer command queue, return real handle) ----
semi_handle_t semi_player_open(const char * /*src*/) { return 0; }
semi_handle_t semi_player_play(void) { return 0; }
semi_handle_t semi_player_pause(void) { return 0; }
semi_handle_t semi_player_seek(long long /*position_us*/) { return 0; }
semi_handle_t semi_player_close(void) { return 0; }
int           semi_player_set_volume(unsigned int /*volume*/) { return 0; }

// ---- Queries (TODO: read ApiLayer session-state snapshot) ----
int       semi_player_get_state(int * /*out_state*/) { return 0; }
long long semi_player_get_duration(void) { return 0; }

// ---- Handle ----
int semi_player_handle_cancel(semi_handle_t /*handle*/) { return 0; }

// ---- Progress ----
int semi_player_progress_subscribe(semi_progress_cb /*cb*/, void * /*user_data*/) { return 0; }

} // extern "C"

