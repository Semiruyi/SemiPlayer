# seek 命令的编排设计

> 属于 ApiLayer 模块。描述 `seek` 命令在 ApiLoop 中如何编排执行。
> 总体原则（世代号机制）见 `docs/architecture.md`。
> 各模块内部 seek 响应细节见各自模块文档（demuxer.md / video_decoder.md / audio_decoder.md / audio_clock.md，待设计）。

## Context

seek 是播放器里最复杂的命令：它跨越整条管道（解封装→解码→时钟），且 FFmpeg 的 seek 行为有副作用（定位到最近关键帧、解码器内部有参考帧残留）。本文档定义 **ApiLoop 如何编排 seek 的执行**，以及为什么需要这几步。

**基准**：seek 是昂贵操作，使用方应清楚成本并主动用 cancel 管理频率（见 api_layer.md）。播放器忠实执行、完成才 resolve。

---

## seek 命令的编排（ApiLoop 内）

ApiLoop 处理 seek 命令时，**顺序调用 5 个模块**（线性，无需 SeekCoordinator）：

```
void handle_seek(pos):
    demuxer.seek(pos)            // ① 解封装定位 + 推进世代号
    video_decoder.seek(pos)      // ② 视频解码器 seek
    audio_decoder.seek(pos)      // ③ 音频解码器 seek
    audio_resampler.seek(pos)    // ④ 音频重采样器 seek (flush 内部残留)
    audio_clock.jump_to(pos)     // ⑤ 时钟跳到目标 PTS
    handle.resolve(Ok(()))
```

**为什么不需要 SeekCoordinator**：5 步是线性顺序调用，没有分阶段等待、没有多分支、没有死锁风险（世代号消掉了"先停生产者再清队列"的顺序协调）。一个简单的顺序调用不该包成协调器模块。

---

## 为什么是这 4 步——三层数据正确性分工

seek 要解决三个**不同层面**的正确性问题，分属不同机制。理解这三层，才能理解为什么需要这 4 步、为什么 decoder 不是裸 flush：

| 层 | 问题 | 谁解决 | 机制 |
|----|------|--------|------|
| **① 解码器内部** | 旧参考帧残留（B帧依赖） | decoder | `avcodec_flush_buffers()` |
| **② 跨 seek 的旧数据** | 上次播放的包/帧还留在队列里 | 世代号 | 数据带 generation，消费者丢弃旧世代 |
| **③ 同次 seek 内、目标前的帧** | FFmpeg 定位到最近关键帧（≤目标），解出目标前的帧 | decoder | PTS 过滤（`frame.pts < target` 丢弃） |

### 第①层：解码器内部残留

`avcodec_flush_buffers()` 清空解码器内部参考帧缓存，重置状态。**这是 flush 做的事，但它只做这个**——它不会让解码器"跳到某个 PTS"，flush 后解码器是空状态，等下一个关键帧从头解。

→ 包含在 decoder.seek() 内部。

### 第②层：跨 seek 的旧数据（世代号）

上次播放残留在 PacketQueue / FrameStore 里的旧数据，靠**世代号**让消费者自动丢弃。Demuxer 定位后、读新数据前 `generation+1`，新数据标新世代，旧世代数据被消费者丢弃。

→ 由 Demuxer.seek() 推进 generation（**generation+1 与定位绑定，定位完成后才 +1，保证 generation 永远对应定位后的新数据**）。各 decoder/sink 看世代号自洽，零协调。

### 第③层：同次 seek 内、目标前的帧

**FFmpeg 的 seek 定位到 ≤目标的最近关键帧**（如 seek 100s，可能定位到 98s 关键帧）。于是 decoder 会先解出 98s、99s（在目标之前）的帧。这些帧世代号是对的（同一次 seek），但 PTS 在目标之前——**世代号管不了这层**。

如果不处理：视频会闪一下 98s/99s 的画面，音频会"咝"一下杂音。

→ 由 decoder.seek() 记下 `target_pts`，后续解码时 `frame.pts < target_pts` 的帧直接丢弃，不输出到下游。

**这就是为什么 decoder 的 seek 不是简单 flush**——flush 解决第①层，PTS 过滤解决第③层，缺一不可。

---

## 各步的职责（高层，不含内部实现）

| 步骤 | 模块方法 | 高层职责 | 内部细节归属 |
|------|---------|---------|------------|
| ① | `demuxer.seek(pos)` | 停旧读 + `av_seek_frame` 定位 + `generation+1` + 读新数据 | demuxer.md（待设计）|
| ② | `video_decoder.seek(pos)` | flush 内部状态 + 记 target_pts + 后续丢弃 < target 的帧 | video_decoder.md（待设计）|
| ③ | `audio_decoder.seek(pos)` | 同视频（音频也要丢 < target，否则杂音） | audio_decoder.md（待设计）|
| ④ | `audio_resampler.seek(pos)` | flush 重采样器内部残留样本（防跨 seek 串音）+ gen 丢旧 | audio_resampler.md（待设计）|
| ⑤ | `audio_clock.jump_to(pos)` | 时钟跳到目标 PTS（连续标量，不能靠丢弃识别） | audio_clock.md（待设计）|

---

## 关键设计决策记录

### generation+1 必须在定位之后、与"读新数据"绑定
不能在 demuxer.seek 开头就 +1，否则定位过程中读到的旧包会被标成新世代。+1 与定位完成、切到读新数据绑定（在 demuxer.seek 内部原子完成）。

### decoder 需要专门的 seek()，不是裸 flush
flush 只清内部参考帧（第①层）。还需 PTS 过滤丢目标前的帧（第③层）。decoder.seek() = flush + 记 target + 后续过滤。

### decoder 的 target_pts 与 generation 需原子一致
decoder 读 `(generation, target_pts)` 时不能撕裂（否则读到新 generation 配旧 target）。各 decoder 内部用原子整体快照（如 std::atomic<std::shared_ptr>）保证一致。→ 内部实现细节，归 video_decoder.md。

### 时钟单独 jump_to
时钟是连续标量，世代号的"丢弃识别"对它无效，必须显式跳到目标 PTS。

### 不需要 SeekCoordinator
线性 4 步顺序调用，无分阶段等待、无死锁。简单调用不该包协调器。

---

## 边界（本文档不涉及）

- ❌ 各模块 seek 内部实现（flush 怎么调、target_pts 怎么存、std::atomic<std::shared_ptr> 怎么用）→ 各模块文档
- ❌ seek 命令的取消语义、频率控制 → api_layer.md
- ❌ 世代号机制的总体原则 → architecture.md
