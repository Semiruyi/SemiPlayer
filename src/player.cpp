#include "player.hpp"

namespace semi {

namespace {
bool g_initialized = false;
}

int player_init() {
    if (g_initialized) return 0; // 幂等
    // TODO: IoCContainer::assemble()（按 DAG 拓扑构造所有模块 + 注入 shared_ptr）
    // TODO: ApiLoop::spawn()（启动命令执行线程）
    g_initialized = true;
    return 0;
}

int player_shutdown() {
    if (!g_initialized) return 0; // 幂等
    // TODO: ApiLoop::stop()（命令执行线程退出）
    // TODO: IoCContainer::dispose()（手动逆序释放，ApiLayer/ApiLoop 最先）
    g_initialized = false;
    return 0;
}

} // namespace semi
