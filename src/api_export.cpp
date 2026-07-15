#include "semi_player/semi_player.h"

// C ABI 导出层（也是生命周期入口）。
// init/shutdown 将直接调用 IoCContainer：assemble() 构造所有模块 + 注入 shared_ptr 并取得 ApiLayer；
// shutdown → dispose() 逆序释放。控制命令将转投 ApiLayer 命令队列。
// IoC 尚未落地，当前为桩。对应接口分类见 docs/modules/api_layer/api_layer.md。
//
// 导出只由头文件里的 SEMI_API（dllexport）声明负责：编译本目标时
// SEMI_PLAYER_DLL_EXPORT 已定义，故头中声明带 dllexport，定义处无需再标。

extern "C" {

// ---- Lifecycle ----
// TODO(IoC): init     → IoCContainer::assemble() + ApiLoop::spawn()
//            shutdown → ApiLoop::stop() + IoCContainer::dispose()
int semi_player_init(void) {
    return 0;
}

int semi_player_shutdown(void) {
    return 0;
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

