# play / pause 命令的编排设计

> 属于 ApiLayer 模块。描述 `play`/`pause` 命令在 ApiLoop 中如何编排执行。
> 各模块内部启动/暂停响应细节见各自模块文档（待设计）。

## Context

- **open** 准备好播放前提（除数据）：探测 + 配置解码器、状态=Ready，管道是"冷的"。
- **play** 让数据流动起来：冷启动管道、（首次）填到水位、解冻时钟 + 出声出画。
- **pause** 冻结播放：切静音、冻结时钟、停显示，但**不拆管道**——下游停消费后靠背压自然停上游，使 resume 是热的。

首次 play 有冷启动延迟（队列从空开始填）；pause 后再 play 是热启动（管道还在、队列有数据）。

---

## play 命令的编排（ApiLoop 内）

```
fn handle_play():
    // ① 边界处理
    if player_state == Playing:
        handle.resolve(Ok(())); return        // 已在播, 无操作
    if player_state == Ended:
        target_start_pts = 0                   // 播完再 play 从头
        // 视同冷启动流程 (见下)

    // ② 启动管道 (从上游到下游)
    demuxer.start_reading()                    // 开始解封装, 填 PacketQueue
    video_decoder.start()                      // 开始解码
    audio_decoder.start()

    // ③ (首次/Ended重置时) 填水位, 不立即出声
    if 是冷启动 (Ready→Playing):
        wait_until(                            // 等水位达标
            AudioFrameStore 水位 >= 阈值 且 VideoFrameStore 有合适帧
        )

    // ④ 解冻 + 出声出画
    audio_clock.unfreeze()                     // 时钟开始推进
    audio_sink.start_playback()                // cpal 取音频出声 (同时更新 AudioClock)
    video_sync.start()                         // VideoSync 按时钟贴帧

    player_state = Playing
    handle.resolve(Ok(()))
```

### target_start_pts 的消费（首次启动时定位）

`demuxer.start_reading()` 内部检查 `target_start_pts`：
- 若 `target_start_pts != 0`：先 `av_seek_frame(target_start_pts)` + `generation+1`，再开始读新位置数据。（对应 open.md 里"Ready 态 seek 调起点"的真正落地）
- 若 `== 0`：从头读。

这样 Ready 态 seek 设的起点，在 play 启动 demuxer 时真正生效。

### "等水位"的实现要点

③ 步的 `wait_until` 不能让 ApiLoop **死循环空转**（会卡住后续命令）。实现为：play 注册一个"水位达标"的条件/回调，被 AudioFrameStore/VideoFrameStore 的水位监听唤醒后才 resolve。ApiLoop 在此期间应能继续处理其他命令，或 play 命令自身挂起让出执行权。

→ 这是 play 实现的并发细节，归 ApiLoop 内部，本文档不展开。

---

## pause 命令的编排（ApiLoop 内）

```
fn handle_pause():
    if player_state != Playing:
        handle.resolve(Ok(())); return         // 非播放态, 无操作

    // ① 下游停消费
    audio_sink.stop_playback()                 // cpal 切静音/暂停, 不取音频
    audio_clock.freeze()                       // 时钟冻结 (带偏移修正, 保证 resume 不跳)
    video_sync.pause()                         // VideoSync 停在当前帧 (纹理保留)

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
| ② | `demuxer.start_reading` | 启动解封装线程；按 target_start_pts 定位（含 generation+1）；填 PacketQueue | demuxer.md |
| ② | `video_decoder.start` | 启动视频解码线程，填 VideoFrameStore | video_decoder.md |
| ② | `audio_decoder.start` | 启动音频解码线程，填 AudioFrameStore | audio_decoder.md |
| ③ | 水位等待 | 等 AudioFrameStore/VideoFrameStore 达水位（仅冷启动）| ApiLoop 内部 |
| ④ | `audio_clock.unfreeze` | 时钟解冻，开始推进 | audio_clock.md |
| ④ | `audio_sink.start_playback` | cpal 开始取音频出声，更新 AudioClock | audio_sink.md |
| ④ | `video_sync.start` | VideoSync 按 AudioClock 贴帧 | video_sync.md（待设计）|

## pause 各步的职责

| 步骤 | 模块方法 | 高层职责 |
|------|---------|---------|
| ① | `audio_sink.stop_playback` | cpal 暂停/切静音，不取音频 |
| ① | `audio_clock.freeze` | 时钟冻结（偏移修正，resume 不跳）|
| ① | `video_sync.pause` | 停在当前帧，纹理保留 |
| — | （不动 demuxer/decoder）| 背压自然停 |

---

## 关键设计决策

### 首次 play 冷启动填水位再出声
open 没预填，首次 play 时队列从空开始。为避免出声时数据不足导致 underrun（断续），play **先填到水位再 start_playback**。出声时已缓冲好，不会断续。代价：首次 play 的 resolve 稍慢（等水位）。这是冷启动固有的、一次性的延迟。

### pause 后再 play 是热启动（快）
只有 Ready→Playing 是冷启动（填水位）。Paused→Playing 是热的：管道还在跑、队列有数据，play 只需解冻时钟 + 恢复消费，瞬间响应。

### pause 靠背压自然停上游
pause 不主动停 demuxer/decoder 线程。下游停消费→队列满→上游阻塞。resume 即热。符合"缓冲满自然停"哲学。

### 时钟 freeze/unfreeze 的偏移修正
pause 时记录暂停时刻，freeze 时钟（PTS 不再推进）。resume 时把暂停时长累加进时钟偏移基准，保证 PTS 连续不跳。详见 audio_clock.md（待设计）。

### Ended 态再 play 从头
播放到结尾（Ended）后再 play，重置 `target_start_pts=0`，走冷启动流程从开头播。

---

## 状态机（play/pause 相关）

```
Ready ──play(冷启动,填水位)──▶ Playing
                                  ⇅
                              play/pause
                                  ⇅
                              Paused ──play(热启动)──▶ Playing

Ended ──play(重置从头,冷启动)──▶ Playing
```

- Ready→Playing：冷启动（填水位）
- Paused↔Playing：热启动（解冻/冻结）
- Ended→Playing：重置 + 冷启动

---

## 边界（本文档不涉及）

- ❌ 各模块 start/pause/freeze 内部实现 → 各模块文档
- ❌ 水位阈值的具体值、水位监听的并发实现 → ApiLoop 内部 / 各 FrameStore 文档
- ❌ open 的上下文建立 → open.md
- ❌ seek 的编排（Playing/Paused 态 seek）→ seek.md
