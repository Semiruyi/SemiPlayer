# play / pause 命令的编排设计

> 属于 ApiLayer 模块。描述 `play`/`pause` 命令在 ApiLoop 中如何编排执行。
> 各模块内部启动/暂停响应细节见各自模块文档（待设计）。

## Context

- **open** 准备好播放前提：探测 + 配置 Demuxer/AudioDecoder/AudioResampler/AudioOutput，状态=Ready。
- **play** 打开最下游 AudioOutput 的消费阀门，使 playback frame store 开始被消费，音频链路开始流动。
- **pause** 关闭 AudioOutput 的消费阀门，但**不拆管道**——下游停消费后靠背压自然停上游，使 resume 是热的。

首次 play 有冷启动延迟（队列从空开始填）；pause 后再 play 是热启动（管道还在、队列有数据）。

---

## play 命令的编排（ApiLoop 内）

```
void handle_play():
    // ① 边界处理
    if player_state == Playing:
        handle.resolve(Ok(())); return        // 已在播, 无操作
    if player_state == Ended:
        target_start_pts = 0                   // 播完再 play 从头
        // 视同冷启动流程 (见下)

    // ② 打开音频输出消费阀门
    audio_output.start_playback()

    player_state = Playing
    handle.resolve(Ok(()))
```

### 起始位置

Demuxer 在 `open()` 成功后自动启动读包，不由 play 单独启动。起始位置定位由
`seek()` 命令完成，具体 backend 定位能力待 `DemuxerBackend` 契约完善后实现。

### 当前阶段的水位策略

当前实现先不做 play 阶段预填水位。`play()` 只打开 AudioOutput 消费阀门，让已经配置好的
Demuxer / AudioDecoder / AudioResampler / AudioOutput 通过队列背压自然流动。后续如果要减少首次出声
underrun，可以在 ApiLayer 或专门的预缓冲协调模块中加入水位等待，但不改变本阶段的控制边界。

---

## pause 命令的编排（ApiLoop 内）

```
void handle_pause():
    if player_state != Playing:
        handle.resolve(Ok(())); return         // 非播放态, 无操作

    // ① 下游停消费
    audio_output.pause_playback()              // AudioOutput 暂停消费 playback store

    // ② 不主动停 demuxer/decoder!
    //    下游停消费 → 队列填满 → demuxer/decoder 阻塞在 cv 上 (背压自然停)

    player_state = Paused
    handle.resolve(Ok(()))
```

### 为什么 pause 不停 demuxer/decoder

下游（AudioSink/VideoSync）停消费后，PacketQueue/FrameStore 会填满，demuxer/decoder 在 push 时阻塞在条件变量上——**背压自然停上游**。这正符合"缓冲满自然停"的设计哲学。

好处：resume 时管道是热的，队列里已有缓冲数据，瞬间恢复，无冷启动延迟。

---

## play 各步的职责（高层，不含内部实现）

| 步骤 | 模块方法 | 高层职责 | 内部细节归属 |
|------|---------|---------|------------|
| ② | `audio_output.start_playback` | 允许 AudioOutput 消费 playback frame store，音频链路开始流动 | audio_output.md |

## pause 各步的职责

| 步骤 | 模块方法 | 高层职责 |
|------|---------|---------|
| ① | `audio_output.pause_playback` | 暂停消费 playback frame store，不清队列、不 reset |
| — | （不动 demuxer/decoder）| 背压自然停 |

---

## 关键设计决策

### 首次 play 暂不做预填水位
当前阶段先实现最小闭环：play 打开 AudioOutput 消费，数据从上游自然流动。预填水位、音画同步和时钟冻结属于后续播放质量层的编排。

### pause 后再 play 是热启动（快）
只有 Ready→Playing 是冷启动（填水位）。Paused→Playing 是热的：管道还在跑、队列有数据，play 只需解冻时钟 + 恢复消费，瞬间响应。

### pause 靠背压自然停上游
pause 不主动停 demuxer/decoder 线程。下游停消费→队列满→上游阻塞。resume 即热。符合"缓冲满自然停"哲学。

### 时钟 freeze/unfreeze 的偏移修正
当前阶段还没有接入 AudioClock；pause/play 只控制 AudioOutput 消费。后续接入 AudioClock 时，
pause 需要冻结时钟，resume 需要修正暂停偏移，保证 PTS 连续不跳。

### Ended 态再 play 从头
当前 Demuxer 在 `Exhausted` 或 `Failed` 后不能继续生产；播放到结尾或发生错误后，
需要由 ApiLayer 先完成当前媒体的 close，再重新 open，之后 Demuxer 会自动开始新的读包会话。

---

## 状态机（play/pause 相关）

```
Ready ──play(打开 AudioOutput 消费)──▶ Playing
                                  ⇅
                              play/pause
                                  ⇅
                              Paused ──play(恢复 AudioOutput 消费)──▶ Playing

Ended ──play(重置从头,冷启动)──▶ Playing
```

- Ready→Playing：打开 AudioOutput 消费，音频链路开始流动
- Paused↔Playing：关闭/恢复 AudioOutput 消费
- Ended→Playing：重置 + 冷启动

---

## 边界（本文档不涉及）

- ❌ 各模块 start/pause/freeze 内部实现 → 各模块文档
- ❌ 水位阈值的具体值、水位监听的并发实现 → ApiLoop 内部 / 各 FrameStore 文档
- ❌ open 的上下文建立 → open.md
- ❌ seek 的编排（Playing/Paused 态 seek）→ seek.md
