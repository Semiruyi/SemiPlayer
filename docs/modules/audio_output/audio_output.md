# AudioOutput 模块设计

> 音频输出模块。位于 `AudioResampler` 之后，负责从 playback PCM store 消费已经适配设备格式的音频帧，
> 将其提交给真实音频设备或输出后端，并在设备缓冲实际 drain 完成后声明播放结束。

`AudioOutput` 延续 Demuxer、AudioDecoder、AudioResampler 的 worker 模型：worker 线程属于模块生命周期，由
IoC 装配时创建、释放时停止并 join；媒体会话通过 `configure()` / `unconfigure()` 表达。它不暴露
模块生命周期意义上的 `start()` / `stop()` / `seek()`。播放/暂停只控制 AudioOutput 是否从 playback store
消费数据；seek 数据隔离由共享 `Generation` 表达。

## Context

AudioResampler 的输出已经是播放链路希望消费的 PCM 格式，例如：

```text
48000Hz / stereo / f32 / packed
```

AudioOutput 不再做解码或重采样。它负责探测/打开输出设备，选择播放链路最终要使用的 playback PCM 格式，
并把符合该格式的 PCM 稳定交给设备边界。设备侧的自然结束和失败由 AudioOutput 转化为上层可观察事件。

```text
AudioPacketQueue(gen)
  -> AudioDecoder
  -> AudioFrameStore(gen, decoded PCM / EndOfInput)
  -> AudioResampler
  -> AudioFrameStore(gen, playback PCM / EndOfInput)
  -> AudioOutput
  -> AudioOutputBackend
  -> OS / device
```

代码层面继续复用 `AudioFrameStore`。文档和 IoC 装配中需要区分两个角色：

- decoded frame store：AudioDecoder 输出，AudioResampler 输入。
- playback frame store：AudioResampler 输出，AudioOutput 输入。

## 格式来源

音频链路里有三类格式/配置，不能混在一起：

```text
Demuxer.open()
  -> AudioCodecConfig             // encoded stream config，给 AudioDecoder

AudioDecoder.configure(codec)
  -> decoded AudioPcmFormat       // decoder 实际输出 PCM，给 AudioResampler input

AudioOutput.configure(options)
  -> playback AudioPcmFormat      // 设备/策略选择的播放 PCM，给 AudioResampler output
```

- Demuxer 探测的是容器和编码流信息，例如 codec、extradata、采样率、声道数等。
- AudioDecoder 根据 encoded config 打开解码器，并在 configure 完成后产出真实 decoded PCM format。
- AudioOutput 根据系统设备、默认策略或宿主选项选择 playback PCM format。
- AudioResampler 只消费纯数据：`decoded_format -> playback_format`，不直接依赖 AudioDecoder 或 AudioOutput 模块实例。

## 职责

AudioOutput 负责：

- 从 playback `AudioFrameSource` 拉取 `AudioFrame` 和 `AudioFrameEndOfInput`。
- 检查 generation，丢弃过期 frame 和过期 EOF。
- 将当前 generation 的 PCM frame 提交给 `AudioOutputBackend`。
- 处理 backend 背压：设备缓冲暂不可写时等待 backend progress 通知。
- 收到当前 generation EOF 后，等待 backend drain 完成，再发出播放完成事件。
- generation 改变时丢弃 pending frame，reset backend，避免旧音频继续播放。
- backend 失败时进入 Failed，并向上发送结构化失败事件。

AudioOutput 不负责：

- 不解码压缩音频包。
- 不做重采样、sample format 转换或 planar/packed 转换。
- 不选择音频流。
- 不把目标播放格式交给上游猜测；AudioOutput 是 playback PCM format 的来源。
- 不直接管理 Demuxer、AudioDecoder 或 AudioResampler 的生命周期。
- 不向上暴露设备缓冲细节，例如 frame consumed、buffer empty、drain started。

## 对外接口

AudioOutput 的公开接口分两类：媒体会话配置，以及播放消费阀门控制。worker 生命周期仍不对外暴露：

```cpp
struct AudioOutputOptions {
    std::optional<std::string> device_id;
};

struct AudioOutputConfigureResult {
    contracts::media::AudioPcmFormat playback_format;
};

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    [[nodiscard]] virtual std::expected<AudioOutputConfigureResult, AudioOutputError>
    configure(const AudioOutputOptions& options) = 0;

    [[nodiscard]] virtual std::expected<void, AudioOutputError> start_playback() = 0;

    [[nodiscard]] virtual std::expected<void, AudioOutputError> pause_playback() = 0;

    virtual void unconfigure() noexcept = 0;
};
```

| 方法 | 调用时机 | 语义 |
|---|---|---|
| `configure(options)` | open | 投递 ConfigureCommand 并同步等待完成；打开/选择输出设备，产出 playback PCM format；进入 Configured；成功后默认暂停，不消费 playback frame store |
| `start_playback()` | play | 恢复 backend 设备并允许 worker 从 playback frame store 消费；幂等 |
| `pause_playback()` | pause | 暂停 backend 设备和 worker 消费；不清队列、不 reset backend、不推进 generation；失败返回 backend 错误 |
| `unconfigure()` | close | 投递 UnconfigureCommand 并同步等待完成；结束当前媒体会话，释放设备输出上下文，回到 Constructed；幂等且 `noexcept` |

没有 `start()` / `stop()`：

- worker 线程生命周期与模块对象一致。
- `configure()` 只建立媒体会话上下文，backend 初始为 paused；`start_playback()` 恢复设备并打开消费阀门。
- `unconfigure()` 只结束当前媒体会话，不销毁 worker。
- `pause_playback()` 同时暂停 backend 设备和 AudioOutput 消费；下游停止消费后，上游通过有界队列背压自然停住。

没有 `seek()`：

- ApiLayer 不需要向 AudioOutput 转发 seek 命令。
- Demuxer/open/seek 成功推进共享 `Generation`。
- AudioOutput worker 在数据面发现 generation 改变后 reset backend，并丢弃旧 generation 数据。

## 依赖关系

### 构造期注入

| 依赖 | 用途 |
|---|---|
| `AudioFrameSource` | 输入端口；消费 playback frame store 中的播放格式 PCM 和 EOF |
| `Generation` | 与 Demuxer、AudioDecoder、AudioResampler 共享；用于丢弃旧媒体会话或旧 seek 数据 |
| `Notifier` | 接收输入 Store 非空、backend 可推进通知；发送 AudioOutput 自身事件 |
| `AudioOutputBackend` | 纯设备输出后端抽象；当前预期可由 miniaudio、WASAPI、SDL 或 fake backend 实现 |
| 内部 `AudioPlaybackClockState` | AudioOutput 拥有并在实际播放进度处更新；通过 `current_position()` 向 VideoSync 暴露只读位置 |

### configure 注入/返回的纯数据

- `AudioOutputOptions`：输出设备选择和偏好策略。第一版可以为空或只表达默认设备。
- `AudioOutputConfigureResult::playback_format`：AudioOutput 最终选择的播放格式，也是 AudioResampler 的 output format。

AudioOutput 不运行时依赖 AudioResampler 模块本身。它只消费 resampler 写入的 playback frame store，并在 configure
阶段向 open 编排返回纯数据格式，保持 DAG 单向。

### 不依赖什么

- 不依赖 AudioResampler 模块实例，只依赖其输出 Store 的 consumer port。
- 不依赖 AudioDecoder 或 Demuxer 模块实例。
- 领域层和契约层不暴露 miniaudio、WASAPI、SDL、FFmpeg 等具体后端类型。
- 不依赖 ApiLayer 回调来驱动数据消费；ApiLayer 只负责 configure/unconfigure 和接收业务事件。

## Backend 契约

后端只处理“设备输出上下文”和“提交播放格式 PCM”。它不关心 Store、Generation、worker、ApiLayer 或上游模块。

建议第一版采用非阻塞 submit/drain：

```cpp
enum class AudioOutputSubmitStatus {
    Accepted,
    WouldBlock,
};

enum class AudioOutputDrainStatus {
    Drained,
    WouldBlock,
};

class AudioOutputBackend {
public:
    virtual ~AudioOutputBackend() = default;

    [[nodiscard]] virtual std::expected<AudioOutputConfigureResult, AudioOutputBackendError>
    configure(const AudioOutputOptions& options) = 0;

    [[nodiscard]] virtual std::expected<void, AudioOutputBackendError> pause() = 0;

    [[nodiscard]] virtual std::expected<void, AudioOutputBackendError> resume() = 0;

    [[nodiscard]] virtual std::expected<AudioOutputSubmitStatus, AudioOutputBackendError>
    try_submit(const contracts::media::DecodedAudio& audio) = 0;

    [[nodiscard]] virtual std::expected<AudioOutputDrainStatus, AudioOutputBackendError>
    try_drain() = 0;

    [[nodiscard]] virtual std::expected<void, AudioOutputBackendError> reset() = 0;
    virtual void unconfigure() noexcept = 0;
};
```

后端操作语义：

- `configure()`：打开/选择设备，建立输出上下文，但不启动设备 callback，并返回最终 playback PCM 格式。
- `pause()`：停止设备 callback；保留 backend ring 中尚未播放的 PCM；已 paused 时幂等成功。
- `resume()`：启动设备 callback；保留并继续消费 pause 前已经提交的 PCM；已 running 时幂等成功。
- backend 在构造时注入并持有 `shared_ptr<AudioOutputRealTimeNotifier>`；通知目标在整个 backend 生命周期内保持不变。
- `try_submit(audio)`：尝试提交一段 PCM；成功返回 `Accepted`，设备缓冲暂不可写返回 `WouldBlock`。
- `try_drain()`：EOF 后尝试确认设备侧缓冲已经播放/排空；未完成返回 `WouldBlock`，完成返回 `Drained`。
- `reset()`：generation 变化时停止并等待可能报告旧缓冲消费量的 callback 完成，再丢弃后端内部待播放旧数据；reset 返回后 worker 才提交新的 active generation，并保持 reset 前的 paused/running 状态。
- `unconfigure()`：释放设备资源。

不建议第一版使用阻塞式 `write()`。阻塞式接口虽然简单，但会让 `unconfigure()`、析构和错误恢复被设备阻塞影响，测试背压也不如非阻塞契约清晰。

### Backend 实时通知

backend 在设备回调中发布统一的实时事件，而不是持有 AudioOutput 的专用回调接口：

```cpp
using AudioOutputRealTimeNotifier = RealTimeNotifier<
    RealTimeEventSpec<std::uint32_t, 1>>;
```

`AudioOutputRealTimeNotifier` 的事件类型当前为 `std::uint32_t`，表示确认消费的 PCM frame 数，且只注册一个
sink：`DefaultAudioOutput::ProgressSink`。它读取 `active_generation`，更新内部的
`AudioPlaybackClockState`，设置原子进度提示并唤醒自己的 worker；worker 再根据 phase 决定推进
`try_submit()` 还是 `try_drain()`。`AudioPlaybackClockState` 不直接订阅 realtime notifier。

只有设备回调真正从 PCM ring 读出的媒体帧会发布确认消费帧数通知；欠载时补出的静音不会发布事件，因此不会错误推进音频时钟。通知器的注册、冻结和解绑规则见 `notifier/realtime_notifier.md`。

## 事件通知

AudioOutput 是音频流水线里第一个真正应该向上声明“播放自然结束”的模块，因为只有它知道 playback store EOF 已经到达，并且设备缓冲已经 drain 完。

建议事件：

```cpp
struct AudioPlaybackFinished {
    Generation::Value generation;
};

struct AudioOutputBackendFailure {
    Generation::Value generation;
    AudioOutputBackendError error;
};
```

不建议发送：

- 不发送 `FrameConsumed`。
- 不发送 `BufferEmpty`。
- 不发送 `DrainStarted`。
- 不发送 `InputEnded`。

这些都是输出链路内部细节。宿主通常只关心播放结束或播放失败。

## 状态机与线程

AudioOutput 和 Decoder/Resampler 一样拆成两个正交状态机：

```text
WorkerState（模块生命周期）
Starting --Started--> Alive --ShutdownRequested--> ShuttingDown --Stopped--> Stopped

SessionState（媒体会话）
                 backend 失败
Constructed --configure()--> Configuring --> Configured ------------------> Failed
     ^                         |             |                              |
     |                         |             |                              |
     +--------- unconfigure() -+-------------+----------- unconfigure() ----+
```

`Configured` 下再维护一个数据面 phase：

```cpp
enum class PlaybackPhase {
    Running,
    Draining,
    Finished,
};
```

- `Running`：正常消费 playback frame store，并向 backend submit。
- `Draining`：已经收到当前 generation 的 EOF，停止读取新 frame，反复尝试 `backend.try_drain()`。
- `Finished`：backend drain 完成并发送 `AudioPlaybackFinished`；等待 unconfigure 或 generation 改变。

输入为空、backend WouldBlock、播放暂停都不是独立 session state，只是 `Configured` 下的等待条件。

## 命令通道

`configure` / `unconfigure` 由 worker 执行，而不是调用方直接访问 backend：

```cpp
struct ConfigureCommand {
    AudioOutputOptions options;
    std::promise<std::expected<AudioOutputConfigureResult, AudioOutputError>> completion;
};

struct UnconfigureCommand {
    std::promise<void> completion;
};

using ControlCommand = std::variant<ConfigureCommand, UnconfigureCommand>;
```

- 命令只由 worker 线程处理。
- backend 的 `configure/try_submit/try_drain/reset/unconfigure` 始终由 worker 独占调用。
- worker 醒来后优先处理控制命令，再推进数据面。
- `configure()` 失败经 completion 返回 `AudioOutputError`。
- 运行期 backend 失败发送 `AudioOutputBackendFailure` 并进入 `Failed`。

## 工作循环

worker 醒来后优先处理控制命令，再处理数据：

```text
如果有 control command:
  取出并执行
否则如果 session != Configured:
  wait

否则如果 generation 改变:
  reset = backend.reset()
  如果 reset 失败:
    发送 AudioOutputBackendFailure
    session = Failed
    等待 unconfigure()
  清空 pending frame
  active_generation = current_generation
  phase = Running

否则如果 phase == Finished:
  wait

否则如果有 pending frame:
  尝试 backend.try_submit(pending frame)
  Accepted: 清除 pending frame，继续推进
  WouldBlock: 等待 backend progress

否则如果 phase == Draining:
  尝试 backend.try_drain()
  Drained: 发送 AudioPlaybackFinished，phase = Finished
  WouldBlock: 等待 backend progress

否则从 AudioFrameSource try_pop():
  Empty: 等待 AudioFrameStoreNotEmpty
  stale generation: 丢弃
  AudioFrame: 保存为 pending frame，然后尝试 submit
  AudioFrameEndOfInput: phase = Draining，然后尝试 drain
```

pending frame 是必要的：backend 可能因为设备缓冲满而暂时拒绝当前 frame。pending frame 保存“已经从输入 Store 取出，但尚未成功提交给设备”的有序数据，保证背压下不丢帧、不越过 EOF。

## 背压与通知

AudioOutput 订阅：

- playback frame store 的 `AudioFrameStoreNotEmpty`：输入可读时唤醒。
- backend progress：设备缓冲可能可写或 drain 状态可能变化时唤醒。

AudioOutput 发送：

- `AudioPlaybackFinished`。
- `AudioOutputBackendFailure`。

不发送 playback store empty/full 事件；这些是 Store 自己负责的资源状态。

### 睡眠与 hint 正确性

AudioOutput 可以沿用 AudioDecoder / AudioResampler 的 worker 自有锁 + 非阻塞端口 + hint 协议：

```cpp
AudioFrameSource::try_pop()
AudioOutputBackend::try_submit()
AudioOutputBackend::try_drain()
```

worker 只持有自己的 `mutex_` 检查：

- `worker_state_`
- `session_state_`
- `commands_`
- `pending_frame_`
- `phase_`
- `input_not_empty_hint_`
- `backend_progress_hint_`

Notifier 回调也必须先获取同一把 `mutex_`，设置对应 hint，然后 `cv.notify_one()`。因此在 `cv.wait(lock, predicate)` 的 predicate 检查和真正进入等待之间，不存在其他线程偷偷把 hint 从 `false` 改成 `true` 却丢失唤醒的窗口。

hint 的含义仍然是“worker 有理由做一次非阻塞尝试”：

- `true` 可以不精确，最多导致多尝试一次。
- `false` 必须可靠，表示没有未消费的可重试信号，worker 才能安心睡眠。

submit/drain 失败后不能无条件覆盖新的通知。正确做法是先在 worker mutex 下消费 hint，再释放锁执行非阻塞尝试；如果尝试期间 notifier 已经把 hint 重新置为 `true`，worker 重新取锁后应保留这个新信号。

## Generation 与 seek

AudioOutput 不提供公开 `seek()`，但必须正确响应 generation 变化。

worker 发现 `generation.current()` 与 `active_generation_` 不一致时：

1. 调用 backend `reset()`，停止并等待旧 generation 的设备 callback，再清空设备缓冲。
2. 丢弃 pending frame。
3. 更新 `active_generation_`。
4. 将 phase 重置为 `Running`。
5. 只处理当前 generation 的 frame 和 EOF。

旧 generation frame 直接丢弃。旧 generation EOF 也直接丢弃，不得触发 `AudioPlaybackFinished`。

这和内部 `AudioPlaybackClockState` 的约束配套：AudioOutput 丢弃旧 generation 数据时不应校准时钟；只有当前 generation 且实际提交/播放的数据才能参与时钟校准。

## EOF 与播放结束

`AudioFrameEndOfInput` 是有序 Store item，不是 Notifier 事件。

AudioOutput 消费到当前 generation 的 EOF 时：

1. 确认 EOF 前面的普通 frame 已经按 FIFO 顺序提交给 backend。
2. 进入 `Draining`。
3. 反复尝试 `backend.try_drain()`。
4. backend 返回 `Drained` 后，发送 `AudioPlaybackFinished{generation}`。
5. 进入 `Finished`，不再消费更多输入，直到 generation 改变或 unconfigure。

收到 EOF 不等于播放完成。播放完成必须等设备后端确认内部缓冲已经播放/排空。

## 内部 AudioPlaybackClockState

AudioOutput 拥有 `AudioPlaybackClockState`，因为它是唯一同时掌握 PCM 时间线、backend reset、
设备暂停和实际消费 callback 的边界。它不作为独立 IoC 资源，也不接受 ApiLayer 的直接调用。

- AudioOutput 只用当前 generation 的真实播放数据更新内部时钟，并通过 `current_position()` 提供只读播放位置给 VideoSync。
- 丢弃旧 generation 数据或 reset backend 时，内部时钟失效旧锚点。
- seek 不调用独立时钟 API：AudioOutput 预读并保留新 generation 首块 PCM 后建立 Prepared 锚点；实际消费事件再建立运行锚点。
- pause/resume 由 `pause_playback()` / `start_playback()` 在控制 backend 的同一顺序内冻结或恢复内部时钟。

## 对现有架构的连带影响

1. 新增 `AudioOutput` worker 和 `AudioOutputBackend` 契约。
2. IoC 创建 playback `AudioFrameStore` 并注入给 AudioOutput。
3. AudioResampler 输出 sink 指向 playback frame store。
4. open 编排按格式来源组织：
   - `demuxer.open(source)` 返回 encoded audio config。
   - `audio_decoder.configure(encoded_config)` 返回 decoded PCM format。
   - `audio_output.configure(options)` 返回 playback PCM format。
   - 配置 AudioResampler(decoded PCM format, playback PCM format)。
5. close 编排增加 `audio_output.unconfigure()`。
6. seek 编排不调用 `AudioOutput::seek()`；只推进共享 Generation，并由 AudioOutput 自行 reset/drop stale。
7. VideoSync 和 ApiLayer 接收 `AudioPlaybackFinished`：前者在音频先结束时继续调度视频尾帧，后者与 `VideoPlaybackFinished` 汇聚为宿主可见的会话完成事件。

## 关键设计决策

### 使用 AudioOutput 命名

当前文档中曾出现 AudioSink/AudioRenderer。第一版建议统一使用 `AudioOutput` 作为 domain worker 名称：

- `Sink` 更像纯被动写入端口，但本模块是自持线程、主动消费 Store 的 worker。
- `Renderer` 容易和视频渲染概念混淆。
- `Output` 更准确表达“音频输出设备边界”。

后续如果需要区分实时设备输出与文件输出，可以在 backend 或更高层策略里扩展，而不是先拆多个 worker 名称。

### AudioOutput 产出 playback format

播放格式属于输出设备边界，而不是 Demuxer 或 Decoder 的职责。Demuxer 只能探测 encoded stream config；
Decoder 可以产出 decoded PCM format；最终设备愿意稳定消费什么格式，应由 AudioOutput/backend 根据系统设备和策略决定。

AudioResampler 因此不询问 AudioOutput 模块实例，只接收 open 编排传入的两个纯数据格式：

```text
AudioDecoderConfigureResult.decoded_format
AudioOutputConfigureResult.playback_format
```

### 非阻塞 backend 而不是阻塞 write

非阻塞 submit/drain 与现有队列/store 背压模型一致：

- worker 每次只推进一步。
- WouldBlock 通过 backend progress 通知恢复。
- `unconfigure()` 和析构不长期卡在设备写入里。
- 测试可以稳定模拟设备缓冲满、drain 未完成和恢复。

### EOF 走数据，不走通知

EOF 继续作为 `AudioFrameEndOfInput` 在 Store 中有序传播。AudioOutput 只在设备 drain 完后发业务事件。这样上游不需要知道“播放结束”语义，也不会把“读完/解码完/重采样完”和“听完”混在一起。

### pause/resume 同时控制设备和消费阀门

pause/resume 不拆管道，也不清空 backend 缓冲。它同时控制设备 callback 和
`DefaultAudioOutput` 是否继续从 playback store 消费：

- `start_playback()`：调用 backend `resume()`，再打开消费阀门。
- `pause_playback()`：调用 backend `pause()`，成功后关闭消费阀门。

pause 后设备不再从 ring 消费，已经提交但尚未播放的 PCM 会保留；下游停止拉取后，
playback store 会逐渐填满并通过背压让上游自然停住。唯一例外是 seek 后：AudioOutput 在
观察到新 generation 后可预读并保留一块首个有效 PCM，用于内部 `AudioPlaybackClockState` 的 Prepared
锚点；它不得在暂停时持续填充 backend。内部 `AudioPlaybackClockState` 与该顺序一起冻结或恢复。

## 当前实现范围

第一版建议实现：

- `AudioOutput` 契约。
- `AudioOutputBackend` 契约。
- `DefaultAudioOutput` worker。
- `MiniaudioAudioOutputBackend` infrastructure 后端：
  - 固定输出 `48000Hz / stereo / F32 / packed`。
   - 使用 miniaudio playback device callback 从内部 ring buffer 取样本，不足时补静音。
   - configure 后保持设备 paused；play/pause 通过 resume/pause 控制设备 callback。
   - `try_submit()` 非阻塞写入 ring buffer，满时返回 `WouldBlock`。
  - `try_drain()` 在 ring buffer 清空后返回 `Drained`。
  - 第一版暂不实现 `device_id` 选择。
- Fake / Null backend 测试：
  - configure/unconfigure。
  - configure 返回 playback PCM format。
  - 输入为空等待。
  - backend WouldBlock 背压。
  - pending frame 顺序。
  - EOF 后 drain。
  - drain 完发送 `AudioPlaybackFinished`。
   - generation 变化 reset/drop stale。
   - pause/resume 保留 ring/pending 数据，paused 状态下 generation reset 不恢复设备。
   - backend failure 进入 Failed 并发送事件。

暂不实现：

- 设备枚举和设备选择。
- 音量控制。
- latency 统计。
- 多输出设备。

## 测试范围

- `DefaultAudioOutput`：
  - 构造启动 worker，析构完整退出。
  - configure/unconfigure 幂等与非法状态。
  - 输入为空时睡眠，输入到达后唤醒。
  - backend WouldBlock 时保留 pending frame，progress 后继续提交。
  - EOF 不立即 finished，必须 drain 完才发送 `AudioPlaybackFinished`。
  - stale frame 和 stale EOF 被丢弃。
  - generation 变化时调用 backend reset，丢弃 pending，phase 回到 Running；reset 失败会发送 `AudioOutputBackendFailure` 并进入 `Failed`。
  - backend configure/pause/resume/reset/submit/drain 失败返回结构化错误。
- `AudioOutputBackend` contract：
  - submit/drain 状态枚举和错误结构可表达设备背压与失败。
- `MiniaudioAudioOutputBackend`：
   - 未配置时拒绝 pause/resume/submit/drain。
   - 默认设备可用时 configure/submit/reset/drain/unconfigure。
   - 默认设备可用时 pause/resume 保留已提交但未播放的 ring 数据。
  - 默认设备不可用时跳过真实设备 smoke。
  - 暂不支持 `device_id` 选择。
- 端到端：
  - AudioResampler -> playback frame store -> AudioOutput，验证 generation、EOF 顺序和自然结束事件。

## 边界

- 不规定真实音频 API。miniaudio、WASAPI、SDL 等只属于 infrastructure backend。
- 不规定完整设备能力探测策略。第一版可以由 backend 选择一个稳定默认 playback PCM format，例如默认设备支持的 preferred format。
- 不把播放时钟拆成独立模块或暴露给 ApiLayer 写入。
- 不把 Demuxer/Decoder/Resampler 的 EOF 事件暴露给宿主；宿主只观察最终播放完成。
