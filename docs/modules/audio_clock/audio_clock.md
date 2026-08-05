# AudioClock 模块设计（当前版）

> 本文以当前代码为准，描述 AudioClock 的目标契约和接入方式。
> AudioClock 目前还没有实现；本文不是现状报告，而是下一阶段实现依据。

## 1. 当前背景

播放器需要一个统一的音频播放位置，供未来的 VideoSync 按 PTS 选择视频帧。
当前音频链路已经具备声卡消费通知，但还没有把这些通知转换成可查询的播放时钟。

当前真实的数据路径是：

```text
AudioFrameStore
    ↓
DefaultAudioOutput worker
    ↓  try_submit(DecodedAudio)
AudioOutputBackend 的 PCM ring buffer
    ↓
miniaudio playback callback
    ↓  AudioFramesConsumed{frames}
AudioOutputRealTimeNotifier
```

这里有几个必须明确的术语边界：

- 当前没有 `AudioSink` 类；音频消费模块是 `DefaultAudioOutput`。
- 当前没有名为 `AudioResampledStore` 的类型；IoC 中使用第二个 `AudioFrameStore` 保存重采样后的 PCM。
- `DecodedAudio::pts_us` 是媒体时间线上的微秒 PTS。
- `AudioFramesConsumed` 只表示 callback 实际从 backend buffer 读出的 PCM frame 数。
- callback 补出的静音不是媒体数据，不产生 `AudioFramesConsumed`。

## 2. 模块定位

AudioClock 是第 0 层的无 worker 资源，不负责拉起线程，也不决定播放哪些数据。

```text
                         控制面
ApiLayer ───────────────────────────────┐
  open/close: reset                     │
  play: resume                          │
  pause: pause                          ▼
                                   AudioClock
DefaultAudioOutput ── PCM 时间锚点 ──────┤
miniaudio callback ── 实际消费 frame ─────┤
                                         │
                                   current_pts()
                                         ▲
                                      VideoSync
```

AudioClock 的职责：

- 维护当前媒体音频 PTS；
- 以实际被声卡消费的 PCM 为播放进度依据；
- 在 pause、EOF、backend failure 后停止时间推进；
- 在 seek 或新媒体会话后丢弃旧的时间锚点；
- 向 VideoSync 提供线程安全的 `current_pts()` 查询。

AudioClock 不负责：

- 从 `AudioFrameStore` 取 PCM；
- 调用 miniaudio 或控制声卡；
- 处理 generation 对应的数据丢弃；
- 选择或提交视频帧；
- 处理音量、变速或音频重采样。

## 3. 时钟语义

### 3.1 主时钟来源

AudioClock 以实际消费的音频 frame 为主时钟，而不是以提交给 backend 的 frame 为主时钟。

原因是提交只代表 PCM 进入了播放 buffer，不能代表已经听到。pause 时 backend buffer
保留但设备停止，时钟也必须停止；backend reset 清空旧 buffer 后，旧 PCM 不能继续推进时钟。

在两次 callback 通知之间，AudioClock 可以用 `std::chrono::steady_clock` 对最近一次精确
消费位置做短距离插值，从而让 VideoSync 不被 callback 周期量化。插值只是查询层的估计，
下一次实际消费通知到达后重新对齐。

### 3.2 时间单位

- 对外播放位置统一使用微秒 `std::int64_t pts_us`。
- PCM 的 `frames` 是每声道的采样帧数，不是所有声道样本数之和。
- 推进速度由输出采样率决定：

```text
audio_duration_us = consumed_frames × 1'000'000 / sample_rate
```

实现时应避免通过反复浮点累加推进时间；应使用累计 frame 数或整数乘除计算。

### 3.3 没有有效音频时的查询

`current_pts()` 建议返回 `std::optional<std::int64_t>`：

- 没有打开媒体；
- 已配置但还没有首个带 PTS 的 PCM；
- seek 后新 generation 的 PCM 尚未到达；

这些情况下返回 `std::nullopt`，而不是伪造 `0`。VideoSync 可以据此等待有效音频时钟。

`jump_to(position_us)` 后，在新 PCM 到达前，可以暂时返回目标位置作为 provisional position；
首个新 generation PCM 实际消费后，再用真实 PTS 建立锚点并修正该位置。

## 4. 内部状态

AudioClock 不是工作模块，但有一个内部播放阶段：

```text
Reset
  ↓ reset(sample_rate)
WaitingForAudio
  ↓ 首个有效 PCM 被实际消费
Running  ↔  Paused
  ↓ EOF / backend failure
Finished
```

### Reset

没有可用的播放时间。`sample_rate` 可以已经配置，但没有当前音频锚点。

### WaitingForAudio

已经 open 或 seek，且已经知道目标 generation，但新的 PCM 还没有实际消费。
时钟不会使用 wall clock 自行向前推进。

### Running

有有效锚点，且播放允许推进。查询值由最近一次 callback 的精确位置加短距离插值得到。

### Paused

保存 pause 时的当前位置。wall clock 继续流逝，但 `current_pts()` 返回固定位置。

### Finished

当前 generation 的 backend 已经 drain 完成。时钟停止推进，直到下一次 `reset()` 或 `jump_to()`。

backend failure 不需要增加 AudioClock 自己的错误状态；ApiLayer 或 AudioOutput 的错误状态
负责停止它。

## 5. 时间锚点与实际消费

单靠当前的 `AudioFramesConsumed{frames}` 无法知道 PTS，因此需要两类输入：

1. `DefaultAudioOutput` 提供 PCM 的时间锚点；
2. miniaudio callback 提供实际消费的 frame 数。

逻辑上，当前 generation 的第一个有效 PCM 块包含：

```text
anchor_pts_us = PCM block 的 pts_us
sample_rate   = output playback format 的 sample_rate
```

当 callback 实际消费 `N` 个 frame 后：

```text
played_pts_us = anchor_pts_us + consumed_frames × 1'000'000 / sample_rate
```

后续 PCM 块默认沿当前 generation 的音频时间线连续排列；其 PTS 主要用于建立首个锚点
和处理新 generation。若未来需要支持音频时间线中的显式 gap 或 discontinuity，再增加
segment 队列，不在第一版引入。

### 提交与消费的顺序

时间锚点必须和 PCM 进入 backend buffer 的顺序一致：

- `try_submit()` 接受一块 PCM 后，输出 worker 必须把这块 PCM 的 PTS 元数据提供给 clock；
- callback 只报告实际从 buffer 读出的 frame 数；
- `WouldBlock`、backend error 或 generation 不匹配的 PCM 不能成为新的时间锚点；
- 新 generation 的第一块有效 PCM 到达后，旧锚点必须已经失效。

这里的关键不是让 callback 读取 PTS，而是保证“PCM 字节进入 buffer”和“时间元数据进入
clock”具有同样的顺序。实现时需要用测试覆盖 submit 与 callback 并发的边界。

当前 `AudioOutputBackend::try_submit()` 只接收 `DecodedAudio`，并没有把 PCM 字节和时序
元数据作为一个原子提交操作。因此，实现不能机械地假设“`try_submit()` 返回后再通知
clock”就一定没有竞态；miniaudio callback 可能在 backend 已经写入 ring 后立即运行。
实现阶段必须选择并验证一种顺序保证：

- 在 PCM 字节对 callback 可见前，先发布可撤销的时间元数据；`WouldBlock` 或 error 时撤销；
- 或者扩展 backend 的提交边界，使 PCM 和时间元数据一起进入可消费队列。

这属于实现契约，不在本文中提前绑定某一种数据结构。

## 6. 建议契约

下面是目标接口的语义草案，具体文件位置和是否采用抽象接口在实现阶段决定：

```cpp
class AudioClock {
public:
    // sample_rate == 0 means that there is no active media session.
    void reset(std::uint32_t sample_rate) noexcept;
    void resume() noexcept;
    void pause() noexcept;

    void jump_to(std::int64_t pts_us,
                 Generation::Value generation) noexcept;

    // Called by DefaultAudioOutput for accepted PCM metadata.
    void on_audio_submitted(std::optional<std::int64_t> pts_us,
                            std::uint32_t frames,
                            Generation::Value generation) noexcept;

    // Called by the AudioFramesConsumed realtime sink.
    void on_audio_frames_consumed(std::uint32_t frames) noexcept;

    void finish() noexcept;

    [[nodiscard]] std::optional<std::int64_t> current_pts() const noexcept;
};
```

约束如下：

- `on_audio_frames_consumed()` 必须是 `noexcept`；
- realtime callback 路径不能加重锁、分配内存或发送普通 Notifier 事件；
- `current_pts()` 可以被 VideoSync 高频调用，不能依赖 AudioOutput worker 的 mutex；
- `reset()`、`pause()`、`resume()`、`jump_to()`、`finish()` 由控制面串行调用；
- `Generation::Value` 只用于使时间锚点失效和拒绝旧 PCM，不把 AudioClock 变成数据队列。

## 7. 控制面生命周期

### Open

当前 `ApiLayer` 的音频配置顺序是：

```text
AudioDecoder.configure()
AudioOutput.configure()       → 得到 playback_format
AudioResampler.configure()    → 使用 playback_format
```

AudioClock 应在获得 playback sample rate 后执行：

```text
clock.reset(output.playback_format.sample_rate)
```

Open 成功后播放器处于 Ready，时钟等待第一次 play 和有效 PCM。

### Play / Resume

```text
audio_output.start_playback()
clock.resume()
```

`start_playback()` 在当前 AudioOutput 中既表示第一次启动，也表示 pause 后恢复。
如果 backend 启动失败，clock 不得进入 Running。

恢复后如果还没有有效 PCM，clock 保持 `WaitingForAudio`；首个 PCM 被消费后才开始推进。

### Pause

```text
audio_output.pause_playback()
clock.pause()
```

只有 backend pause 成功后才冻结 clock。失败时保留原播放器状态和时钟状态。

暂停不清理 playback buffer，不改变 generation，也不重置时间锚点。

### Seek

当前 seek 的数据语义是由 `Demuxer` 推进 generation，其他 worker 在后续处理数据时观察
generation 并 reset 自己的 backend。AudioClock 的处理应为：

```text
demuxer.seek(position_us) 成功
    ↓
active_generation 更新
    ↓
clock.jump_to(position_us, new_generation)
    ↓
AudioOutput 丢弃旧 generation，reset backend buffer
    ↓
新 generation PCM 到达并重新建立时间锚点
```

`jump_to()` 必须立刻清除旧锚点，防止旧 PCM 的消费通知把时钟拉回 seek 前的位置。
如果 seek 发生在 Paused 状态，时钟跳到新位置后仍保持暂停；resume 后从新 generation PCM
重新开始。

`AudioOutputBackend::reset()` 返回成功前，必须保证旧 device callback 不再继续产生有效的
旧 PCM 消费通知。当前 miniaudio 实现依赖 `ma_device_stop()` 建立这个边界；这应作为 backend
行为和测试的明确前提。

### Close

```text
demuxer.close()
audio_decoder.unconfigure()
audio_resampler.unconfigure()
audio_output.unconfigure()
clock.reset(0)
```

close 后时钟不可继续推进。下一次 open 重新配置 sample rate，并从新会话建立锚点。

### EOF 与错误

AudioOutput 在 backend drain 完成后发送 `AudioPlaybackFinished`。ApiLayer 接收当前 generation
的事件后应调用 `clock.finish()`，再把播放器状态转为 Ended。

AudioOutput backend failure、AudioResampler failure 或其他导致播放停止的错误，也必须停止
clock 的推进，避免 VideoSync 在音频已经停止后继续向未来选帧。

## 8. 线程与实时约束

当前 `AudioOutputRealTimeNotifier` 为 `AudioFramesConsumed` 预留两个 sink：

- 一个由 `DefaultAudioOutput` 使用，用来唤醒 worker 处理 backend progress；
- 第二个供 AudioClock 的 realtime sink 使用。

AudioClock 的 realtime sink 必须满足：

- 只做原子计数、时间戳更新和轻量算术；
- 不获取 `DefaultAudioOutput::mutex_`；
- 不调用普通 `Notifier`；
- 不创建 `std::shared_ptr`、容器节点或其他可能分配内存的对象；
- 不依赖 callback 线程之外的 worker 及时运行。

初版实现不采用“callback 每次创建 `std::atomic<std::shared_ptr>` 快照”的方案，因为这会
把潜在分配带入声卡实时线程。具体实现应优先采用预分配状态、原子字段或单向 realtime
计数器，再由查询端计算当前 PTS。

## 9. 测试计划

### 纯 AudioClock 测试

- reset 后 `current_pts()` 没有有效锚点；
- 首个带 PTS 的 PCM 被消费后建立正确位置；
- 按 sample rate 消费 frame，PTS 推进正确；
- 两次消费通知之间的查询不会明显倒退；
- pause 后等待 wall clock，PTS 保持不变；
- resume 后从冻结位置继续；
- jump while running 会立即清除旧锚点；
- jump while paused 保持暂停并返回新位置；
- 旧 generation 的 PCM 不会建立锚点；
- finish 后 PTS 不再继续推进；
- 没有 PTS 的 PCM 不会伪造有效时钟。

### 音频输出集成测试

- `try_submit()` 被接受后，PCM 时间元数据和消费通知顺序一致；
- pause/resume 不清空已经提交的 PCM；
- generation change 会 reset backend 并丢弃旧 PCM；
- backend reset 后旧 callback 不会推进新 generation 的时钟；
- EOF drain 后 clock 进入 Finished；
- backend failure 后 clock 停止推进。

### 后续 VideoSync 测试

- `current_pts() == std::nullopt` 时不选择新视频帧；
- pause 时保持最后一帧；
- seek 后等待新 generation，并按照新 clock 选帧；
- 音频 callback 周期抖动时，视频选择不依赖 worker 唤醒时机。

## 10. 实现顺序与边界

建议按以下顺序落地：

1. 实现无外部依赖的 AudioClock 状态和纯单元测试；
2. 将 AudioClock 注入 IoC，并接入 playback sample rate；
3. 让 DefaultAudioOutput 提供 PCM 时间锚点；
4. 注册第二个 realtime sink，接收 `AudioFramesConsumed`；
5. 接入 ApiLayer 的 open/play/pause/seek/close/EOF/error 生命周期；
6. 最后实现 VideoSync 的时钟读取和选帧策略。

本阶段不涉及：

- miniaudio latency API 的平台细节；
- 音量控制；
- 变速不变调；
- 音频 PTS discontinuity 的完整 segment 队列；
- VideoSync 的完整选帧算法；
- 对外暴露当前播放位置的 C ABI。

## 11. 与旧设计的差异

本版明确废弃以下旧前提：

- 用 `AudioSink` 作为当前模块名；
- 假设存在 `AudioResampledStore` 类型；
- 在 callback 中通过 `provided - buffered` 获取完整声卡 latency；
- 把 `AudioClock` 视为已经实现的资源；
- 使用 `std::atomic<std::shared_ptr>` 作为 realtime callback 的默认状态更新方式；
- 假设 seek 会同步等待所有下游 worker 完成 reset。

旧版的“音频为主时钟”“pause 冻结”“seek 必须清除旧锚点”这些原则仍然保留，
但实现边界改为适配当前的 `DefaultAudioOutput`、backend ring buffer、realtime notifier
和 generation 机制。
