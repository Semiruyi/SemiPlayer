// 模拟宿主进程：只依赖公开 C ABI（与 Flutter/dart:ffi 侧用法一致）。
// 验证 init/shutdown 返回值，以及 IoC 侧 info 日志（未 init 日志库时走 stderr fallback）。

#include "semi_player/semi_player.h"

#include <cstdio>

namespace {

const char* status_name(int status) {
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
    default:
        return "SEMI_STATUS_UNKNOWN";
    }
}

bool expect_ok(const char* step, int status) {
    std::printf("[host] %-14s -> %s (%d)\n", step, status_name(status), status);
    if (status != SEMI_OK) {
        std::fprintf(stderr, "[host] FAIL: %s expected SEMI_OK\n", step);
        return false;
    }
    return true;
}

bool expect_status(const char* step, int actual, int expected) {
    std::printf("[host] %-14s -> %s (%d)\n", step, status_name(actual), actual);
    if (actual != expected) {
        std::fprintf(stderr, "[host] FAIL: %s expected %s\n", step, status_name(expected));
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::printf("=== semi_player host smoke (C ABI) ===\n");
    std::printf("expect: lifecycle SEMI_OK; commands SEMI_ERR_INTERNAL until media is connected\n\n");

    bool ok = true;

    // 1) 首次 init → assemble
    ok = expect_ok("init#1", semi_player_init()) && ok;

    // 2) 命令已由 ApiLayer 工作线程完成；媒体模块未接入，因此预期 Internal。
    const semi_handle_t open_handle = semi_player_open("smoke.mp4");
    if (open_handle == 0) {
        std::fprintf(stderr, "[host] FAIL: open returned invalid handle\n");
        ok = false;
    } else {
        semi_command_result_t result{};
        ok = expect_status("open await", semi_player_handle_await(open_handle, &result),
                           SEMI_ERR_INTERNAL) && ok;
        ok = expect_status("await consumed", semi_player_handle_await(open_handle, &result),
                           SEMI_ERR_INVALID_HANDLE) && ok;
    }

    // 3) 再次 init → 幂等成功，assemble skipped
    ok = expect_ok("init#2 (idempotent)", semi_player_init()) && ok;

    // 4) shutdown → dispose
    ok = expect_ok("shutdown#1", semi_player_shutdown()) && ok;

    // 5) 再次 shutdown → 幂等成功，dispose skipped
    ok = expect_ok("shutdown#2 (idempotent)", semi_player_shutdown()) && ok;

    // 6) 再 init/shutdown 一轮，确认可重复生命周期
    ok = expect_ok("init#3", semi_player_init()) && ok;
    ok = expect_ok("shutdown#3", semi_player_shutdown()) && ok;

    std::printf("\n=== %s ===\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
