# AudioDecoder 模块设计

> 音频解码模块。位于 Demuxer 与 AudioResampler 之间，将压缩音频包解码为原始 PCM。
> 对外控制由 ApiLayer 调用；本模块不负责重采样、声卡输出或播放时钟。

`AudioDecoder` 与 `Demuxer` 共享同一 worker 模型：它是一个自持有 worker 线程的
工作模块，worker 属于模块生命周期（构造时启动、析构时停止并等待退出），不属于
媒体会话。`configure()` 成功后自动开始消费输入；会话结束由 `unconfigure()` 表达，
不销毁线程。输入为空、输出满和暂停都只是背压等待，不涉及线程重建。

## Context

当前 Demuxer 已能从媒体中选择默认音频流，将其压缩包以 `AudioPacket` 写入
`AudioPacketQueue`。下一步需要将这条数据流延伸为可供 AudioResampler 消费的原始
PCM，而不让 FFmpeg 的 `AVCodecContext`、`AVPacket` 或 `AVFrame` 泄漏到领域层。

```
AudioPacketQueue(gen) -> [AudioDecoder] -> AudioFrameStore(gen, raw PCM / EndOfInput)
                                                    |
                                             [AudioResampler]
```

AudioDecoder 只认识“编码配置、压缩包、原始 PCM”。输出设备支持的采样率、声道数和
样本格式是 AudioSink 与 AudioResampler 的职责，不能反向进入 Decoder。

## 依赖关系

### 构造期注入

| 依赖 | 用途 |
|---|---|
| `AudioPacketSource` | 输入端口：消费带 generation 的压缩音频包和有序结束项 |
| `AudioFrameSink` | 输出端口：生产带 generation 的原始 PCM 帧和有序结束项 |
| `Generation` | 与 Demuxer 共享；丢弃旧媒体会话和旧 seek 世代的包，识别当前解码上下文 |
| `Notifier` | 接收队列/Store 状态变化；发送 Decoder 自身事件 |
| `AudioDecoderBackend` | 纯解码后端抽象；当前由 FFmpeg 实现 |

### 谁依赖 AudioDecoder

- **ApiLayer**：在 `open/play/close` 编排中调用其控制接口。
- **IoCContainer**：装配 `AudioDecoder` 与其后端。
- **测试**：以假 Backend 验证线程、背压、generation 与命令语义。

`AudioResampler` **不依赖 AudioDecoder 模块本身**，只消费 `AudioFrameStore`。这使两者
同处 DAG 第 1 层，不形成环。

### 不依赖什么

- 不依赖 Demuxer 实例：只消费其写入的队列。输入结束通过队列中的
  `AudioPacketEndOfInput` 表达，不订阅 Demuxer 的运行时事件。
- 不依赖 AudioSink、miniaudio 或 AudioClock。
- 不依赖 AudioResampler：Decoder 不知道输出设备目标格式。

## PCM 数据契约

跨层传递的 PCM 是纯数据，定义在 `contracts/media`（`media_types.hpp`），不得包含
FFmpeg 类型。已有概念如下：

```cpp
enum class AudioSampleFormat {
    Unknown,
    U8,
    S16,
    S32,
    S64,
    F32,
    F64,
};

struct AudioPcmFormat {
    std::uint32_t sample_rate;
    std::uint32_t channels;
    AudioSampleFormat sample_format;
    bool planar;
};

struct DecodedAudio {
    AudioPcmFormat format;
    std::uint32_t samples_per_channel;
    std::vector<std::vector<std::byte>> planes;
    std::optional<std::int64_t> pts_us;
};
```

领域资源 `AudioFrame` 包装 `DecodedAudio` 和 `Generation::Value`，由
`AudioFrameStore` 保存。`planes` 必须保留 planar/packed 表示：AudioDecoder 不在此处
为了设备格式而做混音、重采样或样本格式转换；这些转换属于 AudioResampler。

当前 `AudioCodecConfig` 只有 codec 名/extradata + 采样率 + 声道数，不保留源声道
布局。后续若需要保留源声道布局，应扩展为不泄漏 FFmpeg 的布局数据契约；不能让
`AVChannelLayout` 穿过 contracts。

## 对外接口

领域接口由 `ApiLayer` 调用，建议如下：

```cpp
struct AudioDecoderConfigureResult {
    contracts::media::AudioPcmFormat decoded_format;
};

class AudioDecoder {
public:
    virtual std::expected<AudioDecoderConfigureResult, AudioDecoderError>
    configure(const contracts::media::AudioCodecConfig& config) = 0;

    virtual void unconfigure() noexcept = 0;
};
```

| 方法 | 调用时机 | 语义 |
|---|---|---|
| `configure` | `open` | 投递 ConfigureCommand 由 worker 执行，调用方同步等待完成（见「命令通道」）；建立解码上下文并返回 decoded PCM format；状态进入 Configured；成功后自动开始消费输入，不额外创建线程 |
| `unconfigure` | `close` 的资源释放阶段 | 投递 UnconfigureCommand 同步等待完成；结束当前媒体会话，释放解码器上下文，回到 Constructed；`noexcept`，重复调用幂等 |

没有 `start()`/`stop()`，也没有 `pause()`。worker 空闲时在条件变量上等待，
成本可忽略；暂停由 AudioSink 停止消费触发下游 Store 满，再自然背压至
AudioDecoder；恢复播放后，`AudioFrameStore` 变为非满并唤醒 Decoder。Decoder 的线程
不因 pause、会话切换而销毁或重建。

## 命令通道（控制面）

`configure`/`unconfigure` 由 worker 执行而非调用方：调用方（ApiLayer 命令线程）把
命令投递进 worker 的命令队列并阻塞等待完成，与 Demuxer 的 ControlCommand 模式一致
（`commands_` 队列 + `std::promise` completion，投递后 `cv_.notify_one()`，调用方
`completion.get()/wait()`）。命令在 worker 循环里串行取出、在锁外执行：

```cpp
struct ConfigureCommand {
    contracts::media::AudioCodecConfig config;
    std::promise<std::expected<AudioDecoderConfigureResult, AudioDecoderError>> completion;
};

struct UnconfigureCommand {
    std::promise<void> completion;
};

using ControlCommand = std::variant<ConfigureCommand, UnconfigureCommand>;
```

- 命令只由 worker 线程处理，不经过 Notifier。backend 的全部调用（configure/
  decode/drain/reset/unconfigure）因此始终由 worker 独占，无并发前提。
- worker 循环：等待 cv 唤醒后**先处理命令队列**，再按 SessionState 决定是否消费
  输入；数据面唤醒（QueueNotEmpty/StoreNotFull）与命令面共用同一个 cv。
- `configure` 成功时经 completion 返回 decoded PCM format，失败时返回 `AudioDecoderError`；
  `unconfigure` 无错误值，`completion.wait()` 即可。

## 状态机与线程

与 Demuxer 相同，状态拆为两个正交的状态机：

```
WorkerState（模块生命周期，只随构造/析构变化）
Starting --Started--> Alive --ShutdownRequested--> ShuttingDown --Stopped--> Stopped

SessionState（媒体会话）
                 backend 失败
Constructed --configure()--> Configuring --> Configured ──────────────▶ Failed
     ▲                          │              │                         │
     │                          │              ├─ 输入空 / 输出满：wait    │
     │                          │              │                         │
     └─────────────────unconfigure()───────────┴─────────────────────────┘
```

- `WorkerState`：worker 只在构造时启动、析构时 join 退出，`unconfigure()` 不销毁线程。
- `Constructed`：没有媒体相关解码器上下文；worker 空闲等待命令。
- `Configuring`：`configure` 命令执行中（backend 建立解码上下文），同步等待完成。
- `Configured`：解码器就绪，worker 消费输入并输出 PCM；输入空、输出满、暂停都只是
  背压等待，不是独立状态。
- `Unconfiguring`：`unconfigure` 命令执行中（backend 释放解码上下文），同步等待完成。
- `Failed`：worker 遇到 backend 或运行时错误；必须 `unconfigure` 后才能重新 `configure`。
  `Failed` 不销毁 worker，也不会自动重试。

worker 空闲时在条件变量上等待，不占用 CPU；线程销毁只发生在模块析构。不要为
`configure/unconfigure` 之外的会话切换创建或销毁线程。

## 背压与通知

`AudioFrameStore` 是单生产者（AudioDecoder）、单消费者（AudioResampler）的资源。
其接口采用 `try_push/try_pop`，不在资源内阻塞；状态边界变化由 Store 发送事件：

- `AudioFrameStoreNotEmpty`：空 -> 非空，唤醒 AudioResampler。
- `AudioFrameStoreNotFull`：满 -> 非满，唤醒 AudioDecoder。

AudioDecoder 订阅：

- `AudioQueueNotEmpty`，输入可读时唤醒；普通 `AudioPacket` 和
  `AudioPacketEndOfInput` 共用这条有序输入通道；
- `AudioFrameStoreNotFull`，输出可写时唤醒；
- 控制命令（`configure`/`unconfigure`）由 worker 的命令队列处理（见「命令通道」），
  不经过 Notifier。

Notifier 回调只在 `DefaultAudioDecoder` 自身的 `mutex_` 下设置轻量 hint 并
`cv.notify_one()`，不解码、不等待其他线程，也不直接访问队列内部锁。

### 睡眠与 hint 正确性

AudioDecoder 不同时持有命令队列、输入队列和输出 Store 的内部锁来检查全局状态。
队列和 Store 仍通过非阻塞端口访问：

```cpp
AudioPacketSource::try_pop()
AudioFrameSink::try_push()
```

worker 只持有自己的 `mutex_` 来检查：

- `worker_state_`
- `session_state_`
- `commands_`
- `pending_outputs_`
- `input_not_empty_hint_`
- `output_not_full_hint_`

这些状态共同决定 worker 是否应该睡眠。进入 `cv.wait(lock, predicate)` 前，worker
持有 `mutex_` 检查 predicate；Notifier 回调若要修改 hint，也必须先取得同一把
`mutex_`。因此在“检查 predicate”与“进入条件变量等待协议”之间，不存在其他线程偷偷把
hint 从 `false` 改成 `true` 却丢失唤醒的窗口。

hint 不表示队列的精确状态，而表示“worker 有理由进行一次非阻塞尝试”：

- `true` 可以不精确：最多导致 worker 多尝试一次 `try_pop()` 或 `try_push()`。
- `false` 必须可靠：表示对应方向没有未消费的可重试信号，worker 才能安心等待未来通知。

为保证 `false` 不覆盖未来通知，worker 必须采用“先消费 hint，再尝试”的协议：

```text
有 pending 输出时：
  持有 decoder mutex，将 output_not_full_hint_ 置为 false
  释放 decoder mutex
  调用 AudioFrameSink::try_push()

  Accepted:
    弹出已提交的 pending 输出；
    如果 pending_outputs_ 仍非空，可在 decoder mutex 下把 output_not_full_hint_ 置为 true，
    表示自己还有理由继续尝试。

  Full:
    不再根据这次失败无条件写 false；
    重新取得 decoder mutex 后检查是否已有新的 AudioFrameStoreNotFull 通知把 hint 置回 true。
    如果没有新的通知，false 才表示当前没有未消费的可重试信号。
```

输入侧同理：

```text
没有 pending 输出、准备读取输入时：
  持有 decoder mutex，将 input_not_empty_hint_ 置为 false
  释放 decoder mutex
  调用 AudioPacketSource::try_pop()

  取到 item:
    处理 item；如希望继续主动探测输入，可把 input_not_empty_hint_ 置为 true。

  空:
    不再根据这次失败无条件写 false；
    等待未来 AudioQueueNotEmpty 通知重新置 true。
```

这个协议的关键不是让 `hint == false` 与队列真实的 empty/full 状态永久一致，而是保证：
如果 worker 消费 hint 后尝试失败，并且之后没有新的边界通知到达，那么 worker 睡眠是安全的；
如果边界通知已经到达，回调会在同一把 `mutex_` 下留下 `hint == true`，使 predicate 重新成立或
唤醒已经等待的 worker。

## Generation 与媒体会话/seek

IoC 创建一个共享的 `Generation` 实例并注入 Demuxer、AudioDecoder 及后续管道模块。
每次成功打开新的媒体会话时，Demuxer 先推进 Generation；成功完成定位后也才推进
Generation。AudioDecoder 不提供 `seek()`；worker 在处理后续输入时自动发现 generation
变化并重置解码上下文。

worker 观察到 generation 变化时：

1. 丢弃队列中的旧世代包；下游消费者也会丢弃旧世代 `AudioFrame`。
2. 清除 FFmpeg 解码器内部残留，且**不输出** flush 前的旧 PCM。
3. 只对新世代包解码。
因此 ApiLayer 不需要向 AudioDecoder 转发 seek 命令，FFmpeg 上下文的 reset 始终由
worker 独占执行。generation 负责隔离旧数据；目标 PTS 过滤若以后需要，应另行设计数据
契约，不通过 AudioDecoder 的公开 `seek()` 方法完成。

## EOF 与错误

AudioDecoder 从 `AudioPacketQueue` 取到队列项后，worker 必须先检查其 generation。旧世代的普通
`AudioPacket` 和 `AudioPacketEndOfInput` 都直接丢弃；只有当前世代的结束项才进入 EOF 处理。
取到当前世代的 `AudioPacketEndOfInput` 后，不能立即宣告结束：AAC 等 codec 可能仍有内部延迟帧。
worker 必须先确认结束项之前的普通包已经处理完，再向 Backend
执行一次 drain（等价于向 FFmpeg 送空 packet 并持续 receive frame），将当前 generation 的
剩余 PCM 全部写入 `AudioFrameStore`，然后将结束项写入同一个 FIFO：

```cpp
struct AudioFrameEndOfInput {
    Generation::Value generation;
};
```

`AudioFrameEndOfInput` 受与 PCM 相同的 FIFO 顺序和容量背压约束。AudioResampler 只在消费到
当前 generation 的结束项后 drain 自己的 `SwrContext`。它表示 Decoder 的输出已经结束，不等同于
Demuxer 的输入结束；最终播放结束由输出链路确认。

后端失败仍经 Notifier 发送 `AudioDecoderBackendFailure`；worker 回到可停止的等待状态，不在通知
回调中执行恢复策略。

## FFmpeg 后端封装

沿用现有 `DefaultDemuxer + DemuxerBackend` 的分层：

```
DefaultAudioDecoder (domain worker)
  -> AudioDecoderBackend (contracts)
       -> FfmpegAudioDecoderBackend (infrastructure)
```

### `DefaultAudioDecoder` 的职责

- 管理 worker、cv、状态机、背压和 Notifier 订阅。
- 检查 generation，在世代变化时重置 backend 并丢弃旧数据。
- 将 Backend 返回的 `DecodedAudio` 包装成 `AudioFrame` 并推入 Store。
- 绝不包含 `AV*` 类型或调用 FFmpeg API。

### `AudioDecoderBackend` 的职责

接口接收 `AudioCodecConfig` 和 `EncodedPacket`，返回纯 `DecodedAudio`；最小能力为：

```cpp
configure(config) -> expected<AudioDecoderBackendConfigureResult, AudioDecoderBackendError>
decode(encoded_packet) -> expected<vector<DecodedAudio>, AudioDecoderBackendError>
drain() -> expected<vector<DecodedAudio>, AudioDecoderBackendError>
reset() noexcept
unconfigure() noexcept
```

Backend 不知道队列、generation、线程、Notifier、播放状态或输出设备。`configure()` 返回的
`decoded_format` 是 decoder 实际会输出的 PCM 格式，是 AudioResampler input format 的权威来源。

### `FfmpegAudioDecoderBackend` 的职责

- 唯一持有 `AVCodecContext`、`AVPacket` 和 `AVFrame`。
- 基于 `AudioCodecConfig` 中的 codec 名称和 extradata 构造 `AVCodecContext`；无法找到
  decoder 或配置失败时返回结构化 Backend 错误。
- 从 `EncodedPacket` 的 payload 与微秒 PTS/DTS 重建输入 `AVPacket`；解码上下文的
  `pkt_timebase` 固定为微秒，避免向领域层泄漏 FFmpeg time base。
- 通过 `avcodec_send_packet/avcodec_receive_frame` 解码，并复制 AVFrame 数据为
  `DecodedAudio`，包括样本格式、planar 标记、采样率、声道数、每声道样本数与 PTS。
  复制长度按有效样本数和每样本字节数计算，不包含 FFmpeg 为对齐加入的行尾填充；复制完成后
  立即复用 AVFrame。
- `drain()` 送空 packet；`reset()` 调 `avcodec_flush_buffers()`；`unconfigure()` 释放所有
  FFmpeg 资源。

`FfmpegAudioDecoderBackend` 不得把 `AVFrame*` 借给 Store，也不得要求上层传入
`AVPacket*`。PCM 数据在 Backend 边界完成所有权转移，保证队列生命周期与 FFmpeg buffer
生命周期互不耦合。

## 测试范围

- `DefaultAudioDecoder`：configure/unconfigure 幂等、输入空等待、输出满背压、空闲等待时
  unconfigure 立即返回、generation 丢弃与 backend reset、EOF drain 与错误事件。
- `FfmpegAudioDecoderBackend`：真实媒体 fixture 的配置、解码 PCM 格式与 PTS、drain、
  无效 codec/extradata 的错误映射。
- 端到端：Demuxer -> AudioPacketQueue -> AudioDecoder -> AudioFrameStore，验证包与帧的
  generation、EOF 与关闭时线程收敛。

## 边界

- `AudioFrameStore` 当前采用有界 FIFO + `mutex`；其具体资源实现独立于 AudioDecoder。
- 不实现 `swr_convert`、输出格式协商或 miniaudio；归 AudioResampler / AudioSink。
- 不实现设备音量、音频时钟或 Flutter 回调。
- 不在本阶段扩展真实 seek 的 DemuxerBackend 契约；Decoder 通过 generation 变化自动响应。
