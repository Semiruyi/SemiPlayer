#pragma once

#include "semi_player/status.h"

#include <cstdint>
#include <memory>
#include <string>

namespace semi::application {

using CommandHandle = std::uint64_t;

// Uninitialized 属于顶层 Player 生命周期，不属于 ApiLayer 会话状态。
enum class PlayerState : std::uint8_t {
    Idle,
    Ready,
    Playing,
    Paused,
    Ended,
    Error,
};

struct MediaInfo {
    std::int64_t duration_us = 0;
    std::uint32_t video_width = 0;
    std::uint32_t video_height = 0;
    bool has_audio = false;
    bool has_video = false;
    bool has_subtitle = false;
};

// await 会把这个结果复制给调用方并消费对应 handle。
struct CommandResult {
    bool has_media_info = false;
    MediaInfo media_info;
};

// 应用层命令中枢。它拥有命令队列、任务句柄表和唯一的命令执行线程。C ABI 仅通过
// 此类投递命令，不接触内部队列或业务模块。
class ApiLayer final {
public:
    ApiLayer();
    ~ApiLayer();

    ApiLayer(const ApiLayer&) = delete;
    ApiLayer& operator=(const ApiLayer&) = delete;
    ApiLayer(ApiLayer&&) = delete;
    ApiLayer& operator=(ApiLayer&&) = delete;

    // stop 停止接收新命令，取消尚未开始的命令，并等待正在执行的命令结束。
    [[nodiscard]] bool start() noexcept;
    [[nodiscard]] bool stop() noexcept;

    // 成功入队返回非零句柄；0 表示未运行、容量已满或入队失败。
    [[nodiscard]] CommandHandle open(std::string source);
    [[nodiscard]] CommandHandle play();
    [[nodiscard]] CommandHandle pause();
    [[nodiscard]] CommandHandle seek(std::int64_t position_us);
    [[nodiscard]] CommandHandle close();
    [[nodiscard]] CommandHandle set_volume(std::uint32_t volume);

    // 等待命令进入终态，将结果复制到 out_result 并消费 handle。返回命令的最终状态；
    // 无效、已消费或已淘汰的 handle 返回 SEMI_ERR_INVALID_HANDLE。
    [[nodiscard]] semi_status_t await(CommandHandle handle, CommandResult& out_result);

    // 仅接受尚未开始的任务的取消请求。任务不会移出队列，而是由命令线程完成为
    // SEMI_ERR_CANCELLED 并通知 await。true 表示请求已被接受。
    [[nodiscard]] bool cancel(CommandHandle handle) noexcept;

    // 仅为实现文件中的队列辅助函数保留的前置声明；具体布局不暴露给调用方。
    struct Impl;

private:

    [[nodiscard]] CommandHandle enqueue_open(std::string source);
    [[nodiscard]] CommandHandle enqueue_play();
    [[nodiscard]] CommandHandle enqueue_pause();
    [[nodiscard]] CommandHandle enqueue_seek(std::int64_t position_us);
    [[nodiscard]] CommandHandle enqueue_close();
    [[nodiscard]] CommandHandle enqueue_set_volume(std::uint32_t volume);

    std::unique_ptr<Impl> impl_;
};

} // namespace semi::application
