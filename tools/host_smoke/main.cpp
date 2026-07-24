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
    case SEMI_ERR_INVALID_RESOURCE:
        return "SEMI_ERR_INVALID_RESOURCE";
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
    std::printf("expect: lifecycle and sample-media open return SEMI_OK\n\n");

    bool ok = true;

    // 1) 首次 init → assemble
    ok = expect_ok("init#1", semi_player_init()) && ok;

    const semi_handle_t invalid_play_handle = semi_player_play();
    if (invalid_play_handle == 0) {
        std::fprintf(stderr, "[host] FAIL: play returned invalid handle\n");
        ok = false;
    } else {
        semi_command_result_t result{};
        ok = expect_status("play while idle",
                           semi_player_handle_await(invalid_play_handle, &result),
                           SEMI_ERR_INVALID_STATE) && ok;
    }

    // 2) 通过公开 C ABI 打开仓库内的真实 FFmpeg 测试媒体。
    const semi_handle_t open_handle = semi_player_open(SEMI_PLAYER_SMOKE_MEDIA_PATH);
    if (open_handle == 0) {
        std::fprintf(stderr, "[host] FAIL: open returned invalid handle\n");
        ok = false;
    } else {
        semi_command_result_t result{};
        ok = expect_ok("open await", semi_player_handle_await(open_handle, &result)) && ok;
        if (!result.has_media_info || !result.media_info.has_video || !result.media_info.has_audio) {
            std::fprintf(stderr, "[host] FAIL: sample media info is incomplete\n");
            ok = false;
        }
        ok = expect_status("await consumed", semi_player_handle_await(open_handle, &result),
                           SEMI_ERR_INVALID_HANDLE) && ok;
    }

    const semi_handle_t close_handle = semi_player_close();
    if (close_handle == 0) {
        std::fprintf(stderr, "[host] FAIL: close returned invalid handle\n");
        ok = false;
    } else {
        semi_command_result_t result{};
        ok = expect_ok("close await", semi_player_handle_await(close_handle, &result)) && ok;
        if (result.has_media_info) {
            std::fprintf(stderr, "[host] FAIL: close returned media info\n");
            ok = false;
        }
    }

    const semi_handle_t reopen_handle = semi_player_open(SEMI_PLAYER_SMOKE_MEDIA_PATH);
    if (reopen_handle == 0) {
        std::fprintf(stderr, "[host] FAIL: reopen returned invalid handle\n");
        ok = false;
    } else {
        semi_command_result_t result{};
        ok = expect_ok("reopen await", semi_player_handle_await(reopen_handle, &result)) && ok;
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
