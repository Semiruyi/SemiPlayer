# close 命令的编排设计

> 属于 ApiLayer 模块。描述 `close` 命令在 ApiLoop 中如何编排执行。
> 各模块内部 close 响应细节见各自模块文档（待设计）。

## Context

close 是 **open 的逆操作**：把播放器从"已绑定媒体"退回"空壳"状态（Idle）。它回收**当前媒体的资源**（解码器、cpal 流、文件句柄、队列数据、停止工作线程），但**模块对象本身不拆**——拆模块是 `shutdown`（lifecycle 层）的事。

**核心边界**：close 回收媒体相关资源，模块对象留着可复用（下次 open 重新 configure 绑定新媒体）。

| | close | shutdown |
|---|---|---|
| 回收什么 | 当前媒体的资源 | 一切（含模块对象 + 全局状态）|
| 之后状态 | Idle（模块还在，可再 open）| Uninitialized（模块没了，要重新 init）|
| 谁调 | ApiLayer 命令 | Player（lifecycle 层）|

---

## close 命令的编排（ApiLoop 内）

```
fn handle_close():
    if player_state in [Idle, Uninitialized]:
        resolve(Ok(())); return          // 没媒体, 无操作

    // ① 停下游消费者
    audio_sink.stop_playback()           // cpal 停止, 切静音
    video_sync.stop()                    // VideoSync 停贴帧

    // ② 停 decoder 线程 (并等退出)
    video_decoder.stop()
    audio_decoder.stop()
    audio_resampler.stop()               // 停重采样线程 (在 audio_decoder 之后, audio_sink 之前)

    // ③ 停 demux 线程 (并等退出)
    demuxer.stop_reading()

    // ④ (所有线程已停, 安全) 清空队列数据
    video_packet_queue.clear()
    audio_packet_queue.clear()
    video_frame_store.clear()
    audio_frame_store.clear()
    audio_resampled_store.clear()

    // ⑤ 释放媒体相关资源 (模块对象留着, 只释放媒体上下文)
    demuxer.close()                      // 关文件, 释放 AVFormatContext
    video_decoder.unconfigure()          // 释放解码器实例 (configure 的逆)
    audio_decoder.unconfigure()
    audio_resampler.unconfigure()        // 释放 SwrContext (configure 的逆)
    audio_sink.teardown()                // 释放 cpal 流 (setup 的逆)
    audio_clock.reset(0)                 // 时钟归零冻结

    // ⑥ 会话状态
    player_state = Idle
    current_media = None
    target_start_pts = 0

    handle.resolve(Ok(()))
```

---

## 为什么是这个顺序

### 从下游往上游停，再释放
顺序是 ①下游消费者 → ②decoder → ③demux → ④清队列 → ⑤释放资源。原因：

- **先停消费者**：下游不取数据了，队列会自然停止被消费。
- **再停 decoder**：不再解码，FrameStore 不再增长。
- **再停 demux**：不再读包，PacketQueue 不再增长。
- **所有线程停了，才清队列 + 释放资源**：避免线程访问已清空/已释放的数据导致崩溃。

这是**安全顺序**（避免 use-after-free），不是为了数据正确性——close 是终结操作，停了就停了，不像 seek 有"清空了又被填回"的竞态。

### 释放资源的步骤是 configure/setup 的逆
⑤ 步：`demuxer.close`（open 的逆）、`decoder.unconfigure`（configure 的逆）、`audio_sink.teardown`（setup 的逆）。模块对象本身不销毁（不 Drop），只释放它持有的媒体相关内部资源。

---

## 关键设计决策

### close 比 seek 简单（无需世代号）
close 是终结操作，之后无数据流动，不需要 seek 那种"识别旧数据"的精细机制。直接"停所有线程 → 清空 → 释放"即可，停了就停了，不会有新数据污染。

### "完成才返回" = 等所有线程停 + 资源释放完
close 的 resolve 要汇聚：所有工作线程确认退出（stop 等线程 join）、资源释放完。这是 close 唯一的并发点——**汇聚所有线程的停止确认**。符合"完成才返回、无中间态"原则。

### 任意状态可 close（不强制先 pause）
close 内部处理任何状态：
- Playing 态 close → 先隐式执行下游停止（切静音、停 VideoSync），再走"停所有线程"。
- Paused 态 close → 直接走"停所有线程"（下游本来就停了）。
- Ended 态 close → 同上。

使用方可随心 close，不要求"先 pause 再 close"。

### 显式清空队列
close 时显式 `clear` 所有队列，让模块干净地进入 Idle 态（不残留旧媒体数据）。虽世代号也能让下次 open 的旧数据作废，但 close 是终结操作，清空更干净彻底，下次 open 面对的是空队列。

### close 不拆模块对象
模块对象（Demuxer/Decoder/Sink 实例）在 close 后**仍然存活**——只释放它们持有的媒体资源（解码器、cpal流、文件句柄）。模块对象本身到 `shutdown` 才销毁。这样 close→open 可复用模块（尤其硬解 device 等昂贵资源），符合 lifecycle 设计。

---

## 各步的职责（高层，不含内部实现）

| 步骤 | 模块方法 | 高层职责 | 内部细节归属 |
|------|---------|---------|------------|
| ① | `audio_sink.stop_playback` | cpal 停止/切静音，不取音频 | audio_sink.md |
| ① | `video_sync.stop` | 停止贴帧 | video_sync.md |
| ② | `video_decoder.stop` | 停视频解码线程，等退出 | video_decoder.md |
| ② | `audio_decoder.stop` | 停音频解码线程，等退出 | audio_decoder.md |
| ③ | `demuxer.stop_reading` | 停解封装线程，等退出 | demuxer.md |
| ④ | `*.clear()` | 清空 PacketQueue/FrameStore 数据 | 各队列文档 |
| ⑤ | `demuxer.close` | 关文件，释放 AVFormatContext（open 的逆）| demuxer.md |
| ⑤ | `video_decoder.unconfigure` | 释放解码器实例（configure 的逆）| video_decoder.md |
| ⑤ | `audio_decoder.unconfigure` | 同上 | audio_decoder.md |
| ⑤ | `audio_sink.teardown` | 释放 cpal 流（setup 的逆）| audio_sink.md |
| ⑤ | `audio_clock.reset(0)` | 时钟归零冻结 | audio_clock.md |

---

## 状态机（close 相关）

```
Ready/Playing/Paused/Ended ──close()──▶ Idle
                                          │
                                          ├── open() ──▶ Ready (复用模块, 重新 configure)
                                          └── shutdown() (lifecycle 层) ──▶ Uninitialized
```

close 后任何会话状态都回到 Idle，模块可被 open 复用。

---

## 与 open 的对称性

| open（建） | close（释放） |
|-----------|--------------|
| `demuxer.open`（建 AVFormatContext）| `demuxer.close`（释放）|
| `decoder.configure`（建解码器）| `decoder.unconfigure`（释放）|
| `audio_sink.setup`（建 cpal 流）| `audio_sink.teardown`（释放）|
| `audio_clock.reset(0)` | `audio_clock.reset(0)`（归零）|
| play 启动的工作线程 | close 的 stop（停线程）|
| play 填的队列数据 | close 的 clear（清队列）|

---

## 边界（本文档不涉及）

- ❌ 各模块 stop/unconfigure/teardown/clear 内部实现 → 各模块文档
- ❌ shutdown（模块体系拆除）→ docs/lifecycle.md
- ❌ 工作线程如何"等退出"（join/完成信号）的并发细节 → 各模块文档
