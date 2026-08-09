# AudioOutput 内部 AudioPlaybackClockState 设计（当前版）

> 本文以当前代码为准，描述 AudioOutput 内部播放时钟的实现契约和接入方式。
> `AudioPlaybackClockState` 已由 `DefaultAudioOutput` 持有并实现；本文描述当前实现契约以及后续 VideoSync 接入方式。

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
    ↓  确认消费帧数（当前事件类型为 `std::uint32_t`）
AudioOutputRealTimeNotifier
    ↓  唯一 sink：DefaultAudioOutput::ProgressSink
active_generation + confirmed frames
    ↓
AudioPlaybackClockState
```

这里有几个必须明确的术语边界：

- 当前没有 `AudioSink` 类；音频消费模块是 `DefaultAudioOutput`。
- 当前没有名为 `AudioResampledStore` 的类型；IoC 中使用第二个 `AudioFrameStore` 保存重采样后的 PCM。
- `DecodedAudio::pts_us` 是媒体时间线上的微秒 PTS。
- 确认消费帧数通知只表示 callback 实际从 backend buffer 读出的 PCM frame 数。
- callback 补出的静音不是媒体数据，不产生确认消费帧数通知。

## 2. 所有权与定位

`AudioPlaybackClockState` 是 `DefaultAudioOutput` 的内部状态，不是独立模块、不是 IoC
资源，也没有自己的控制面生命周期。它不拉起线程，也不决定播放哪些数据。

```text
ApiLayer ── start/pause/seek ──> DefaultAudioOutput
                                      │ owns
                                      ▼
                          AudioPlaybackClockState
                                      │ exposes read-only
                                      ▼
                         AudioOutput::current_position()
                                      ▲
                                  VideoSync
```

AudioOutput 内部时钟的职责：

- 维护当前媒体音频 PTS；
- 以实际被声卡消费的 PCM 为播放进度依据；
- 在 pause、EOF、backend failure 后停止时间推进；
- 在 seek 或新媒体会话后丢弃旧的时间锚点；
- 通过只读 `AudioOutput::current_position()` 向 VideoSync 提供线程安全的播放位置查询。

它不负责：

- 从 `AudioFrameStore` 取 PCM；
- 调用 miniaudio 或控制声卡；
- 处理 generation 对应的数据丢弃；
- 选择或提交视频帧；
- 处理音量、变速或音频重采样。

## 3. 时钟语义

### 3.1 主时钟来源

内部时钟以实际消费的音频 frame 为主时钟，而不是以提交给 backend 的 frame 为主时钟。

原因是提交只代表 PCM 进入了播放 buffer，不能代表已经听到。pause 时 backend buffer
保留但设备停止，时钟也必须停止；backend reset 清空旧 buffer 后，旧 PCM 不能继续推进时钟。

在两次 callback 通知之间，内部时钟可以用 `std::chrono::steady_clock` 对最近一次精确
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

`AudioOutput::current_position()` 返回 `std::optional<PlaybackPosition>`：

- 没有打开媒体；
- 已配置但还没有首个带 PTS 的 PCM；
- seek 后新 generation 的首个 PCM 尚未被 AudioOutput 预读；

这些情况下返回 `std::nullopt`，而不是伪造 `0`。VideoSync 可以据此等待有效音频时钟；有效结果同时携带
`generation` 和 `pts_us`。

运行态 seek 后，在新 generation 的 PCM 实际消费前保持 `std::nullopt`，避免视频早于新音频
跳转。暂停态 seek 后，AudioOutput 预读并保留首块新 PCM 时，`current_position()` 返回该块的
冻结 PTS；首个有效消费事件到达后，再以其真实 PTS 建立运行锚点。

## 4. 内部状态

内部时钟用“锚点阶段”和“设备是否暂停”两个正交状态表达：

```text
Reset
  ↓ configure
WaitingForAudio
  ↓ AudioOutput 预读并保留首个有效 PCM
Prepared
  ↓ 首个有效 PCM 被设备实际消费
Running
  ↓ EOF / backend failure
Finished

设备暂停可覆盖 WaitingForAudio、Prepared 或 Running；暂停只冻结查询值，不清空锚点。
```

### Reset

没有可用的播放时间。`sample_rate` 可以已经配置，但没有当前音频锚点。

### WaitingForAudio

已经 open 或 seek，但新的 PCM 还没有被 AudioOutput 预读。时钟不会使用 wall clock 自行向前推进。

### Prepared

AudioOutput 已在 worker 中取得当前 generation 的首块带 PTS PCM，并将它保留为 pending frame；
它尚未进入设备消费路径。若设备暂停，`current_position()` 返回这块 PCM 的 PTS，供 VideoSync 在
暂停 seek 后选择目标帧；若设备正在运行，仍返回 `nullopt`，直到实际消费事件到达。

### Running

有有效锚点，且播放允许推进。查询值由最近一次 callback 的精确位置加短距离插值得到。

### Paused 覆盖状态

保存当前锚点位置。wall clock 继续流逝，但 `current_position()` 返回固定位置；若锚点阶段为
Prepared，则返回 prepared PCM 的 PTS。

### Finished

当前 generation 的 backend 已经 drain 完成。时钟停止推进，直到下一次 `reset()` 或新时间线
的有效消费事件。

backend failure 不需要增加独立错误状态；AudioOutput 的错误状态负责停止内部时钟。

## 5. 时间锚点与实际消费

内部时钟的唯一实时媒体输入是 backend callback 报告的确认消费帧数，当前通知值为
`std::uint32_t`。backend 不携带 generation、first PTS 或 sample rate；这些元数据由
`DefaultAudioOutput` 在自己的边界上维护：

- `active_generation`：由 AudioOutput 在 backend reset 成功后发布，`ProgressSink` 在 callback
  中读取并传给时钟；
- `first_pts_us`：worker 预读当前 generation 的首个带 PTS PCM 时，通过 `set_first_pts()` 设置；
- `sample_rate`：`configure()` 返回 playback format 后，通过 `reset()` 设置。

收到确认消费帧数后，`AudioPlaybackClockState` 先确认 generation、首个 PTS 和 sample rate
仍然有效，再累计 `consumed_frames`，并记录本次 callback 的 `steady_clock` 时间。运行态查询
以累计消费 frame 换算的 PTS 为基准，再加上最近一次消费通知后的短距离 wall-clock 插值：

```text
consumed_pts_us = first_pts_us + consumed_frames × 1'000'000 / sample_rate
current_pts     = consumed_pts_us + elapsed_since_last_callback
```

暂停态和 Finished 态返回冻结位置，不再继续使用 wall clock 推进。旧 generation 的通知会被
时钟忽略；backend reset 必须在返回前等待旧 callback 停止，因此旧 callback 不会推进新时间线。

### Prepared 锚点

`DefaultAudioOutput` 在普通 worker 线程读到当前 generation 的首块有效 PCM 时，先把该块
保留为 pending frame，并以其 `generation` 和 `pts_us` 写入内部时钟的 Prepared 锚点。这不是
realtime 输入，也不表示已经听到声音。它只用于“设备已暂停时的 seek 定位”；设备运行时，
Prepared 锚点不得让 `current_position()` 提前返回新位置。

Prepared PTS 是“恢复后实际将播放的第一帧”的 PTS，不自动等于用户请求的 seek 位置。若解码
从 seek 前关键点开始而上游没有丢弃目标位置前的音频样本，Prepared 也会忠实反映那个较早的
PTS；需要精确跳到请求位置时，应由音频解码/重采样链在进入 AudioOutput 前完成样本裁剪。

当 pending frame 随后被 backend 接受并由 callback 实际消费，确认消费帧数通知会使用已由
AudioOutput 设置的真实 `first_pts_us` 推进时钟并进入 Running。若该 frame 变 stale、submit 失败或
backend reset，AudioOutput 必须清除 Prepared 锚点。

### backend 的时序责任

`try_submit(DecodedAudio)` 仍然是 PCM 写入入口，但它不能再在返回后单独更新时钟。
backend 只需要在设备实际从 PCM ring 读出媒体帧后报告确认消费帧数；generation、first PTS 和
sample rate 由 AudioOutput 维护，不需要通过 callback sidecar 传给时钟：

- 只有实际读出的媒体帧才能产生确认消费帧数通知；补出的静音不能计入；
- `WouldBlock`、backend error 和 stale PCM 不得产生确认消费帧数通知；
- `reset()`、pause 和 unconfigure 后，已失效的元数据不能生成事件；
- `reset()` 返回前必须等待可能报告旧缓冲消费量的 callback 停止；
- realtime 路径不分配内存、不加 worker mutex。

这样由 AudioOutput 统一维护 generation 和时间锚点，避免输出 worker 与设备 callback 分别
维护时钟元数据造成并发竞态。

## 6. 读写边界

VideoSync 只依赖 `AudioOutput` 的只读接口；时钟写入方法是 `DefaultAudioOutput` 私有实现细节，
不注册为独立服务，也不由 ApiLayer 调用：

```cpp
class AudioOutput {
public:
    [[nodiscard]] std::optional<PlaybackPosition> current_position() const noexcept;
};
```

`DefaultAudioOutput` 持有 `AudioPlaybackClockState`，并通过 `current_position()` 暴露只读结果。
时钟的 `reset`、`pause`、`resume`、`finish`、`set_first_pts` 与
`on_audio_frames_consumed` 均由 AudioOutput 自己的 worker、命令处理和 `ProgressSink` 调用。

约束如下：

- `on_audio_frames_consumed()` 必须是 `noexcept`；
- realtime callback 路径不能加重锁、分配内存或发送普通 Notifier 事件；
- `current_position()` 可以被 VideoSync 高频调用，不能依赖 AudioOutput worker 的 mutex；
- 状态写入由 AudioOutput 内部串行化；ApiLayer 不直接控制时钟；
- `generation` 只来自 AudioOutput 的 Prepared 或消费输入，是不透明的时间线标识；内部时钟不依赖
  `Generation` 模块，也不参与 stale 数据丢弃；
- `set_first_pts` 只能由 AudioOutput worker 在保留首块当前 generation PCM 时调用；它不是
  对 ApiLayer 或 VideoSync 的公开控制接口；
- `on_audio_frames_consumed()` 可在设备 callback 中与控制面并发执行；状态发布须用已验证的
  无锁原子协议，使查询端只看到完整、单调的状态快照。

## 7. 控制面生命周期

### Open

当前 `ApiLayer` 的音频配置顺序是：

```text
AudioDecoder.configure()
AudioOutput.configure()       → 得到 playback_format
AudioResampler.configure()    → 使用 playback_format
```

AudioOutput 在 `configure()` 获得 playback sample rate 后初始化其内部时钟；这是
AudioOutput 的内部步骤，不暴露额外 API。

Open 成功后播放器处于 Ready，时钟等待第一次 play 和有效 PCM。

### Play / Resume

```text
audio_output.start_playback()
```

`start_playback()` 在当前 AudioOutput 中既表示第一次启动，也表示 pause 后恢复。
AudioOutput 必须先恢复内部时钟的插值基准、再恢复设备 callback；上面的调用顺序保证 callback
首次消费时内部时钟不会仍处于 Paused。若 backend 启动失败，AudioOutput 保持内部时钟 Paused。

恢复后若有 Prepared PCM，AudioOutput 先提交该 pending frame；内部时钟在它实际消费前不推进。
若没有有效 PCM，内部时钟保持 `WaitingForAudio`。

### Pause

```text
audio_output.pause_playback()
```

AudioOutput 在 backend pause 成功后冻结内部时钟。失败时保留原播放器状态和时钟状态。

暂停不清理 playback buffer，不改变 generation，也不重置时间锚点。

### Seek

当前 seek 的数据语义是由 `Demuxer` 推进 generation，其他 worker 在后续处理数据时观察
generation 并 reset 自己的 backend。内部时钟不由 ApiLayer 跳点；它只在 AudioOutput
预读或实际消费新时间线 PCM 时观察到新的不透明 generation：

```text
demuxer.seek(position_us) 成功
    ↓
AudioOutput 丢弃旧 generation，reset backend buffer
    ↓
AudioOutput 预读并保留新 generation 的首块 PCM
    ↓
设备暂停：内部时钟进入 Prepared，返回该块冻结 PTS
设备运行：等待实际消费
    ↓
确认消费帧数（由 active_generation 绑定）
    ↓
AudioOutput 内部时钟以真实消费位置进入 Running
```

seek 后，AudioOutput reset 完成到首块新 PCM 被预读前，内部时钟处于 `WaitingForAudio`，
`current_position()` 返回 `nullopt`。暂停状态下进入 Prepared 后返回冻结 PTS；恢复设备并消费首个
新 PCM 后，内部时钟才以真实 PTS 建立运行锚点。

`AudioOutputBackend::reset()` 返回成功前，必须保证旧 device callback 不再继续产生有效的
旧 PCM 消费通知。当前 miniaudio 实现依赖 `ma_device_stop()` 建立这个边界；这应作为 backend
行为和测试的明确前提。

### Close

```text
demuxer.close()
audio_decoder.unconfigure()
audio_resampler.unconfigure()
audio_output.unconfigure()
```

close 后时钟不可继续推进。下一次 open 重新配置 sample rate，并从新会话建立锚点。

### EOF 与错误

AudioOutput 在 backend drain 完成后先停止其内部时钟，再发送 `AudioPlaybackFinished`。
ApiLayer 接收当前 generation 的事件后把播放器状态转为 Ended。

AudioOutput backend failure、AudioResampler failure 或其他导致播放停止的错误，也必须停止
内部时钟的推进，避免 VideoSync 在音频已经停止后继续向未来选帧。

## 8. 线程与实时约束

当前 `AudioOutputRealTimeNotifier` 只有一个 sink，由 `DefaultAudioOutput::ProgressSink` 使用。
它在 backend callback 中读取 `active_generation`，更新内部 `AudioPlaybackClockState`，并设置
worker 的 progress hint。`AudioPlaybackClockState` 不直接注册为 realtime sink。

`ProgressSink` 和内部时钟更新路径必须满足：

- 只做原子计数、时间戳更新和轻量算术；
- 不获取 `DefaultAudioOutput::mutex_`；
- 不调用普通 `Notifier`；
- 不创建 `std::shared_ptr`、容器节点或其他可能分配内存的对象；
- 不依赖 callback 线程之外的 worker 及时运行。

初版实现不采用“callback 每次创建 `std::atomic<std::shared_ptr>` 快照”的方案，因为这会
把潜在分配带入声卡实时线程。具体实现应优先采用预分配状态、原子字段或单向 realtime
计数器，再由查询端计算当前 PTS。

## 9. 测试计划

### AudioPlaybackClockState 单元测试

- reset 后 `current_position()` 没有有效锚点；
- 首个带 PTS 的 PCM 被消费后建立正确位置；
- 按 sample rate 消费 frame，PTS 推进正确；
- 两次消费通知之间的查询不会明显倒退；
- pause 后等待 wall clock，PTS 保持不变；
- resume 后从冻结位置继续；
- paused seek 后，预读首块新 PCM 会进入 Prepared 并返回其冻结 PTS；
- running seek 后，Prepared 不会使 `current_position()` 在实际消费前提前跳转；
- 新 generation 的首个有效消费事件会替换旧锚点；
- paused 状态下 Prepared 不会自行恢复推进；
- seek 后首个 PCM 预读前返回 `nullopt`；
- finish 后 PTS 不再继续推进；
- 没有 PTS 的 PCM 不会伪造有效时钟。

### 音频输出集成测试

- `try_submit()` 被接受后，首个 PTS 与消费通知顺序一致；
- pause/resume 不清空已经提交的 PCM；
- generation change 会 reset backend 并丢弃旧 PCM；
- backend reset 后旧 callback 不会推进新 generation 的时钟；
- paused 状态 generation change 只允许预读并保留首个新 PCM，不得持续填充 backend；
- EOF drain 后内部时钟进入 Finished；
- backend failure 后内部时钟停止推进。

### 后续 VideoSync 测试

- `current_position() == std::nullopt` 时不选择新视频帧；
- pause 时保持最后一帧；
- paused seek 的 Prepared PTS 可选择一次新 generation 视频帧；
- seek 后等待新 generation，并按照新的 `current_position()` 选帧；
- 音频 callback 周期抖动时，视频选择不依赖 worker 唤醒时机。

## 10. 实现顺序与边界

当前实现按以下顺序落地：

1. 在 AudioOutput 内实现 `AudioPlaybackClockState` 和单元测试；
2. 由 `configure()` 接入 playback sample rate，并在 generation 变化时 reset；
3. 让 DefaultAudioOutput 预读首块 PCM 并建立首个 PTS 锚点；
4. 由唯一的 `ProgressSink` 接收 backend 消费通知，同时更新 clock 和 worker progress hint；
5. 在 AudioOutput 的 play/pause/seek generation reset/close/EOF/error 路径更新内部状态；
6. 由 VideoSync 通过 `AudioOutput::current_position()` 读取位置，最后实现选帧策略。

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
- 把 `AudioClock` 视为独立资源或由 ApiLayer 直接控制；
- 使用 `std::atomic<std::shared_ptr>` 作为 realtime callback 的默认状态更新方式；
- 假设 seek 会同步等待所有下游 worker 完成 reset。

旧版的“音频为主时钟”“pause 冻结”“seek 必须清除旧锚点”这些原则仍然保留，
但实现边界改为适配当前的 `DefaultAudioOutput`、backend ring buffer、realtime notifier
和 generation 机制。
