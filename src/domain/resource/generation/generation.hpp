#pragma once

#include <atomic>
#include <cstdint>

namespace semi::domain {

// 由 IoC 共享的媒体管道世代号（见 docs/architecture.md "世代号机制"）。
//
// 不变式：所有跨模块传递的数据（packet / 视频帧 / 音频 PCM 块）携带 generation；
//        所有消费者使用数据前检查 generation，不等则丢弃。漏标或漏检任一处，机制失效。
//
// 职责边界：世代号只管"数据正确性"——保证新媒体会话或 seek 后旧世代数据不与新数据混杂。它不承担
//          "取消进行中的 seek 命令"（那是用 cancel 做的），两者正交。
//          新媒体会话在成功 open 后推进；seek 时 generation+1 与 Demuxer 定位绑定：
//          定位完成后才 +1，保证 generation 永远对应定位后的新数据。
//
// 归类说明：generation 与播放器业务（seek 的数据正确性）强相关，故放 domain/。
// DAG 第 0 层，无线程、无依赖。
class Generation {
public:
    using Value = uint32_t;

    Generation() noexcept;
    ~Generation() = default;

    Generation(const Generation&) = delete;
    Generation& operator=(const Generation&) = delete;

    // 新媒体会话成功 open 后，或 seek 定位完成后推进世代号。
    void bump() noexcept;

    // 当前世代号。
    Value current() const noexcept;

    // 数据携带的 generation 是否对应当前世代（消费者使用数据前检查）。
    bool is_current(Value gen) const noexcept;

private:
    std::atomic<Value> value_{0};
};

} // namespace semi::domain
