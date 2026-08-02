# AudioResampler 模块设计

> 音频重采样模块。位于 AudioDecoder 与 AudioSink/AudioRenderer 之间，把解码得到的原始 PCM
> 转换成播放输出链路希望消费的 PCM 格式。本文描述领域层 worker、契约边界和后端抽象，不涉及
> FFmpeg `SwrContext` 或 miniaudio 的具体调用细节。

`AudioResampler` 与 `AudioDecoder` 共享同一种 worker 模型：worker 线程属于模块生命周期，构造后常驻，
析构时停止并 join；媒体会话通过 `configure()` / `unconfigure()` 表达。输入为空、输出满和播放暂停
都通过 Store 背压自然等待，不通过销毁或重启线程表达。

## Context

AudioDecoder 只负责把压缩音频包解码成原始 PCM。这个 PCM 的格式由媒体流本身决定，例如采样率、
声道数、sample format、planar/packed 表示。播放输出侧能稳定消费的格式则由 AudioSink/设备能力
决定，例如 `48000Hz / f32 / stereo / packed`。

两侧格式不一定一致，所以不能要求 AudioSink 直接支持解码格式，也不应该把输出设备知识塞进
AudioDecoder。AudioResampler 填补这段 gap：

```text
AudioPacketQueue(gen)
  -> AudioDecoder
  -> AudioFrameStore(gen, decoded PCM / EndOfInput)
  -> AudioResampler
  -> AudioFrameStore(gen, playback PCM / EndOfInput)
  -> AudioSink / AudioRenderer
```

代码层面可以继续复用 `AudioFrameStore` 类型。文档和 IoC 装配中需要区分两个角色：

- decoded frame store：AudioDecoder 输出，AudioResampler 输入。
- playback frame store：AudioResampler 输出，AudioSink/AudioRenderer 输入。

除非后续两个 Store 的行为真的分化，否则不新增 `AudioResampledStore` 类型，避免为命名复制一套几乎
相同的资源代码。

## 职责

AudioResampler 负责：

- 将 decoded PCM 转换为 playback PCM。
- 转换采样率，例如 `44100 -> 48000`。
- 转换 sample format，例如 `s16 -> f32`。
- 转换 planar/packed 表示。
- 做必要的声道数/声道布局适配。本阶段至少以不泄漏 FFmpeg 类型的 channel count 为契约边界；更完整的
  channel layout 可以后续扩展到 `contracts::media`。
- 维护重采样后端的流式状态，例如 FFmpeg `SwrContext` 的滤波历史。
- 在 generation 变化时丢弃旧数据、清空 pending output，并 reset/flush 后端。
- 消费当前 generation 的 `AudioFrameEndOfInput` 后 drain 后端，并向输出 Store 写入同 generation 的
  `AudioFrameEndOfInput`。

AudioResampler 不负责：

- 不解码压缩音频包，这是 AudioDecoder 的职责。
- 不播放、不触碰声卡、不驱动 miniaudio 实时回调，这是 AudioSink/AudioRenderer 的职责。
- 不决定目标输出格式。目标格式由 open 阶段的输出设备探测/策略决定，然后作为纯数据传给
  `configure()`。
- 不向宿主声明“播放结束”。EOF 作为有序数据项继续往下游传递，最终播放结束由输出链路确认。
- 不提供公开 `seek()`。seek 通过共享 `Generation` 间接表达。

## 对外接口

AudioResampler 的公开接口只表达媒体会话配置，不表达 worker 启停：

```cpp
class AudioResampler {
public:
    virtual ~AudioResampler() = default;

    [[nodiscard]] virtual std::expected<void, AudioResamplerError>
    configure(const contracts::media::AudioPcmFormat& input_format,
              const contracts::media::AudioPcmFormat& output_format) = 0;

    virtual void unconfigure() noexcept = 0;
};
```

| 方法 | 调用时机 | 语义 |
|---|---|---|
| `configure(input_format, output_format)` | open | 投递 ConfigureCommand 并同步等待完成；建立重采样上下文；进入 Configured；成功后 worker 自动消费输入 |
| `unconfigure()` | close | 投递 UnconfigureCommand 并同步等待完成；结束当前媒体会话，释放重采样上下文，回到 Constructed；幂等且 `noexcept` |

没有 `start()` / `stop()`：

- worker 线程由 IoC 装配出来的模块对象持有，生命周期与模块对象一致。
- `configure()` 成功后自动处理输入。
- `unconfigure()` 只结束当前媒体会话，不销毁 worker。
- pause 由下游停止消费造成输出 Store 满，随后背压到 Resampler。

没有 `seek()`：

- ApiLayer 不需要向 Resampler 发送 seek 命令。
- Demuxer/open/seek 成功推进共享 `Generation`。
- Resampler worker 在消费输入 item 时发现 generation 变化，自行 reset 后端并丢弃旧世代数据。

## 依赖关系

### 构造期注入

| 依赖 | 用途 |
|---|---|
| `AudioFrameSource` | 输入端口；消费 decoded frame store 中的原始 PCM 和 EOF |
| `AudioFrameSink` | 输出端口；生产 playback frame store 中的播放格式 PCM 和 EOF |
| `Generation` | 与 Demuxer、AudioDecoder、后续输出模块共享，用于丢弃旧媒体会话/旧 seek 数据 |
| `Notifier` | 接收输入 Store 非空、输出 Store 非满通知；发送 Resampler 自身故障事件 |
| `AudioResamplerBackend` | 纯重采样后端抽象；当前预期由 FFmpeg 实现 |

### configure 注入的纯数据

- `input_format`：AudioDecoder 输出 PCM 的格式。
- `output_format`：AudioSink/AudioRenderer 在 open 阶段确定的播放目标格式。

`AudioResampler` 不运行时依赖 `AudioSink`。目标格式是 open 阶段确定的静态配置，不是 worker 循环中动态
查询的服务。这样可以避免 “AudioSink 依赖 playback frame store，Resampler 又依赖 AudioSink” 的环。

### 不依赖什么

- 不依赖 AudioDecoder 模块实例，只依赖其输出 Store 的 consumer port。
- 不依赖 Demuxer 模块实例，只依赖共享 Generation。
- 不依赖 miniaudio 实例。
- 领域层和契约层不暴露 `AVFrame`、`AVSampleFormat`、`SwrContext` 等 FFmpeg 类型。

## 后端契约

后端只处理纯 PCM 数据和格式转换，不知道队列、generation、worker、Notifier 或播放状态。

建议契约：

```cpp
class AudioResamplerBackend {
public:
    virtual ~AudioResamplerBackend() = default;

    [[nodiscard]] virtual std::expected<void, AudioResamplerBackendError>
    configure(const contracts::media::AudioPcmFormat& input_format,
              const contracts::media::AudioPcmFormat& output_format) = 0;

    [[nodiscard]] virtual std::expected<std::vector<contracts::media::DecodedAudio>,
                                        AudioResamplerBackendError>
    resample(const contracts::media::DecodedAudio& input) = 0;

    [[nodiscard]] virtual std::expected<std::vector<contracts::media::DecodedAudio>,
                                        AudioResamplerBackendError>
    drain() = 0;

    virtual void reset() noexcept = 0;
    virtual void unconfigure() noexcept = 0;
};
```

`DecodedAudio` 目前承担“PCM audio buffer”的数据角色。虽然名字偏向 decoder 产物，但它不包含 decoder 专属
语义，可以暂时复用。若后续觉得语义不够中性，可以单独重命名为 `PcmAudio`，但不应在第一版 Resampler
里顺手扩大重构范围。

后端操作语义：

- `configure()`：按输入/输出 PCM 格式建立转换上下文。
- `resample(input)`：消费一段 decoded PCM，返回零个或多个 playback PCM frame。
- `drain()`：在当前输入 EOF 后取出后端内部残留样本。
- `reset()`：generation 变化时丢弃内部残留，不输出旧世代样本。
- `unconfigure()`：释放所有后端资源。

## 状态机与线程

Resampler 和 Decoder 一样拆成两个正交状态机：

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

- `WorkerState` 只随构造/析构变化。
- `Constructed`：没有当前媒体会话，没有重采样上下文，worker 空闲等待命令。
- `Configuring`：worker 执行 configure 命令并调用 backend 建立上下文。
- `Configured`：worker 自动消费输入 Store 并生产输出 Store。
- `Failed`：backend 运行时失败；必须 `unconfigure()` 后才能重新 `configure()`。
- 输入为空、输出满、播放暂停都不是独立状态，只是 `Configured` 下的等待条件。

命令由 worker 线程串行执行。调用方投递命令后同步等待 completion，与 AudioDecoder 的命令通道保持一致：

```cpp
struct ConfigureCommand {
    contracts::media::AudioPcmFormat input_format;
    contracts::media::AudioPcmFormat output_format;
    std::promise<std::expected<void, AudioResamplerError>> completion;
};

struct UnconfigureCommand {
    std::promise<void> completion;
};
```

## 工作循环

worker 醒来后优先处理控制命令，再处理数据：

```text
如果有 control command:
  取出并执行

否则如果 session != Configured:
  wait

否则如果有 pending output:
  尝试 push 到输出 AudioFrameSink
  如果 Full，等待 AudioFrameStoreNotFull

否则尝试从输入 AudioFrameSource 取 item:
  如果 Empty，等待 AudioFrameStoreNotEmpty
  如果 item generation != current generation，丢弃
  如果是 AudioFrame，调用 backend.resample()，把结果放入 pending output
  如果是 AudioFrameEndOfInput，调用 backend.drain()，把 drain 结果和 EOF 放入 pending output
```

pending output 是必要的：backend 一次输入可能产生多个输出 frame，而输出 Store 可能中途变满。pending output
保存尚未成功写入下游的有序结果，保证背压下不会丢帧、不会越过 EOF。

## 背压与通知

Resampler 订阅资源状态通知：

- 输入 decoded frame store 的 `AudioFrameStoreNotEmpty`：输入可读时唤醒。
- 输出 playback frame store 的 `AudioFrameStoreNotFull`：输出可写时唤醒。

Resampler 发送自身事件：

- `AudioResamplerBackendFailure`：后端 configure/resample/drain 失败时发送给上层观测。

不发送：

- 不发送 “resample finished”。
- 不发送 “input ended”。
- 不发送 “playback finished”。

结束语义通过数据项传递：

```cpp
struct AudioFrameEndOfInput {
    Generation::Value generation;
};
```

AudioResampler 只有在消费到当前 generation 的输入 EOF，并完成 backend drain 后，才把同 generation 的 EOF 写入
输出 Store。最终是否播放结束由输出链路在实际消费完 playback frame store 后判断。

### 睡眠与 hint 正确性

Resampler 不同时持有命令队列、输入 Store 和输出 Store 的内部锁来检查全局状态。Store 只暴露非阻塞端口：

```cpp
AudioFrameSource::try_pop()
AudioFrameSink::try_push()
```

worker 只持有自己的 `mutex_` 检查：

- `worker_state_`
- `session_state_`
- `commands_`
- `pending_outputs_`
- `input_not_empty_hint_`
- `output_not_full_hint_`

Notifier 回调也必须先获取同一把 `mutex_`，设置对应 hint，然后 `cv.notify_one()`。因此在
`cv.wait(lock, predicate)` 的 predicate 检查和真正进入等待之间，不存在其他线程偷偷把 hint 从 `false`
改成 `true` 却丢失唤醒的窗口。

hint 不是精确队列状态，而是“worker 有理由做一次非阻塞尝试”的许可证：

- `true` 可以不精确，最多导致多一次 `try_pop()` 或 `try_push()`。
- `false` 必须可靠，表示没有未消费的可重试信号，worker 才能安心睡眠。

协议与 AudioDecoder 保持一致：先在自身 mutex 下消费 hint，再释放锁调用非阻塞端口。失败后不根据这次失败
无条件写 false；若失败期间有新的 Notifier 回调，它会在同一把 mutex 下把 hint 重新置为 true，使 predicate
重新成立或唤醒已经等待的 worker。

## Generation 与 seek

Resampler 不提供公开 `seek()`，但必须正确响应 generation 变化。

worker 发现 `generation_->current()` 与 `active_generation_` 不一致时：

1. 清空 pending output，避免继续输出旧世代 PCM 或旧 EOF。
2. 调用 backend `reset()`，丢弃重采样器内部残留样本，避免跨 seek 串音。
3. 更新 `active_generation_`。
4. 丢弃旧 generation 的输入 item。
5. 只处理当前 generation 的 `AudioFrame` 和 `AudioFrameEndOfInput`。

这使 seek 语义保持 generation-only：ApiLayer 不需要协调 Resampler flush，Resampler 也不需要知道 seek 目标
位置。若未来需要 PTS 级别过滤，应设计为数据契约的一部分，而不是恢复公开 `seek()`。

## EOF 与错误

输入 EOF 是有序 Store item，不是 Notifier 事件。Resampler 消费到当前 generation 的
`AudioFrameEndOfInput` 时：

1. 确认 EOF 前面的普通 frame 已按 FIFO 顺序处理。
2. 调用 backend `drain()` 取出内部残留 PCM。
3. 将 drain 结果写入 pending output。
4. 在 pending output 末尾追加同 generation 的 `AudioFrameEndOfInput`。

backend 失败时：

- 清空 pending output 和 hint。
- session 进入 `Failed`。
- 发送 `AudioResamplerBackendFailure`。
- 不自动重试。
- 调用方必须先 `unconfigure()`，再重新 `configure()`。

## 对现有架构的连带影响

1. IoC 新增 `AudioResampler` worker 和 `AudioResamplerBackend`。
2. IoC 创建两个 `AudioFrameStore` 实例：decoded frame store 与 playback frame store。
3. AudioDecoder 的输出 sink 指向 decoded frame store。
4. AudioResampler 的输入 source 指向 decoded frame store，输出 sink 指向 playback frame store。
5. AudioSink/AudioRenderer 后续从 playback frame store 消费。
6. open 编排新增 `audio_resampler.configure(input_format, output_format)`。
7. close 编排新增 `audio_resampler.unconfigure()`。
8. seek 编排不调用 Resampler；只依赖共享 Generation 推进。

## 关键设计决策

### 独立成模块，而不是塞进 AudioDecoder 或 AudioSink

- 不塞进 AudioDecoder：Resampler 的目标格式来自输出设备侧，把这份知识放进 Decoder 会让 Decoder 依赖
  下游策略。
- 不塞进 AudioSink：重采样是有状态计算，可能分配内存，也可能阻塞；不适合放进 miniaudio 实时线程。
- 独立模块 + Store 解耦：每个 worker 只关心自己的输入端口、输出端口和 backend，测试边界清楚。

### 目标格式 open 时确定

目标输出格式是 open 阶段由输出链路探测或策略选择出的静态配置。Resampler configure 后运行期只转换数据，
不再查询 AudioSink。这样 DAG 保持单向：

```text
AudioSink/Output capability -> output_format data -> AudioResampler.configure()
AudioResampler -> playback frame store -> AudioSink/Renderer
```

### 暂不做变速不变调

`swr_convert` 通过改变采样率可以改变播放速度，但会同时改变音调。真正的变速不变调需要 SoundTouch 等时间
伸缩算法。Resampler 是未来接入这类能力的自然位置，但第一版只做格式适配与重采样。

## 测试范围

- `DefaultAudioResampler`：configure/unconfigure 幂等、输入空等待、输出满背压、pending output 顺序、
  EOF drain、generation 变化 reset 和 stale item 丢弃、backend failure 进入 Failed。
- `AudioFrameStore` 复用测试：decoded/playback 两个实例不需要不同资源类型。
- `FfmpegAudioResamplerBackend`：真实 PCM fixture 的格式转换、planar/packed、sample format、采样率转换、
  drain、无效格式错误映射。
- 端到端：AudioDecoder -> decoded frame store -> AudioResampler -> playback frame store，验证 generation、
  EOF 顺序和关闭时线程收敛。

## 边界

- 不实现 `swr_convert` 的缓冲管理、延迟补偿和声道布局细节；归 FFmpeg backend 实现阶段。
- 不实现 AudioSink/AudioRenderer 的设备能力探测。
- 不实现变速不变调。
- 不重命名 `DecodedAudio`；若未来改为 `PcmAudio`，应作为独立契约重构处理。
