# AudioDecoder 模块设计

> 音频解码模块。位于 Demuxer 与 AudioResampler 之间，将压缩音频包解码为原始 PCM。
> 对外控制由 ApiLayer 调用；本模块不负责重采样、声卡输出或播放时钟。

## Context

当前 Demuxer 已能从媒体中选择默认音频流，将其压缩包以 `AudioPacket` 写入
`AudioPacketQueue`。下一步需要将这条数据流延伸为可供 AudioResampler 消费的原始
PCM，而不让 FFmpeg 的 `AVCodecContext`、`AVPacket` 或 `AVFrame` 泄漏到领域层。

```
AudioPacketQueue(gen) -> [AudioDecoder] -> AudioFrameStore(gen, raw PCM)
                                                    |
                                             [AudioResampler]
```

AudioDecoder 只认识“编码配置、压缩包、原始 PCM”。输出设备支持的采样率、声道数和
样本格式是 AudioSink 与 AudioResampler 的职责，不能反向进入 Decoder。

## 依赖关系

### 构造期注入

| 依赖 | 用途 |
|---|---|
| `AudioPacketQueue` | 输入：消费带 generation 的压缩音频包 |
| `AudioFrameStore` | 输出：生产带 generation 的原始 PCM 帧 |
| `Generation` | 丢弃旧世代包，识别 seek 后的新解码上下文 |
| `Notifier` | 接收队列/Store 状态变化和 Demuxer EOF；发送 Decoder 自身事件 |
| `AudioDecoderBackend` | 纯解码后端抽象；当前由 FFmpeg 实现 |

### 谁依赖 AudioDecoder

- **ApiLayer**：在 `open/play/seek/close` 编排中调用其控制接口。
- **IoCContainer**：装配 `AudioDecoder` 与其后端。
- **测试**：以假 Backend 验证线程、背压、generation 与命令语义。

`AudioResampler` **不依赖 AudioDecoder 模块本身**，只消费 `AudioFrameStore`。这使两者
同处 DAG 第 1 层，不形成环。

### 不依赖什么

- 不依赖 Demuxer 实例：只消费其写入的队列。订阅 `DemuxerEndOfStream` 仅是接收
  Notifier 事件，不取得 Demuxer 的运行时引用。
- 不依赖 AudioSink、miniaudio 或 AudioClock。
- 不依赖 AudioResampler：Decoder 不知道输出设备目标格式。

## PCM 数据契约

跨层传递的 PCM 是纯数据，定义在 `contracts/media`，不得包含 FFmpeg 类型。
建议新增以下概念：

```cpp
enum class AudioSampleFormat {
    Unknown,
    U8,
    S16,
    S32,
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

当前 `AudioCodecConfig` 只有声道数。后续若需要保留源声道布局，应扩展为不泄漏
FFmpeg 的布局数据契约；不能让 `AVChannelLayout` 穿过 contracts。

## 对外接口

领域接口由 `ApiLayer` 调用，建议如下：

```cpp
class AudioDecoder {
public:
    virtual std::expected<void, AudioDecoderError>
    configure(const contracts::media::AudioCodecConfig& config) = 0;

    virtual std::expected<void, AudioDecoderError> start() = 0;
    virtual void seek(std::int64_t target_us) noexcept = 0;
    virtual void stop() noexcept = 0;
    virtual void unconfigure() noexcept = 0;
};
```

| 方法 | 调用时机 | 语义 |
|---|---|---|
| `configure` | `open` | 建立解码上下文，状态进入 Idle；不启动线程、不读包 |
| `start` | `play` | 按需创建 worker，开始消费队列；重复调用幂等 |
| `seek` | Demuxer 成功定位并推进 generation 后 | 提交目标 PTS，唤醒 worker 执行 flush 与 PTS 过滤 |
| `stop` | `close/shutdown` | 停止并 join worker；不释放解码器上下文 |
| `unconfigure` | `close` 的资源释放阶段 | 仅在停止后释放解码器上下文，回到 Constructed |

没有 `pause()`。暂停由 AudioSink 停止消费触发下游 Store 满，再自然背压至
AudioDecoder；恢复播放后，`AudioFrameStore` 变为非满并唤醒 Decoder。Decoder 的线程
无需因 pause 销毁或重建。

## 状态机与线程

```
Constructed --configure()--> Idle --start()--> Running
     ^                         ^                 |
     |                         |                 +-- 输入空 / 输出满：wait
     +--unconfigure()-- Stopped <--stop()--------+
```

- `Constructed`：没有媒体相关解码器上下文，未起线程。
- `Idle`：已配置解码器，尚未启动 worker。
- `Running`：worker 存在；它可能正在解码、等待输入或等待输出空间。
- `Stopped`：worker 已 join，保留配置，可再次 `start`；`unconfigure` 后回到
  `Constructed`。

worker 生命周期与 Demuxer 一致：首次 `start` 时创建，直到 `stop/close/shutdown` 才
退出。输入为空、输出满和暂停都只是 wait 条件，不是独立状态。

## 背压与通知

`AudioFrameStore` 是单生产者（AudioDecoder）、单消费者（AudioResampler）的资源。
其接口采用 `try_push/try_pop`，不在资源内阻塞；状态边界变化由 Store 发送事件：

- `AudioFrameStoreNotEmpty`：空 -> 非空，唤醒 AudioResampler。
- `AudioFrameStoreNotFull`：满 -> 非满，唤醒 AudioDecoder。

AudioDecoder 订阅：

- `AudioQueueNotEmpty`，输入可读时唤醒；
- `AudioFrameStoreNotFull`，输出可写时唤醒；
- `DemuxerEndOfStream`，请求 worker drain 解码器；
- 自身 `stop/seek` 控制信号。

Notifier 回调只设置原子标志并 `cv.notify_one()`，不解码、不持有重锁、不等待其他线程。

## Generation 与 seek

Demuxer 完成定位后才推进 Generation。随后 ApiLayer 调用 `audio_decoder.seek(target_us)`；
Decoder 将 `{generation, target_us}` 作为不可撕裂的整体快照提交给 worker。

worker 处理该快照时：

1. 丢弃队列中的旧世代包；下游消费者也会丢弃旧世代 `AudioFrame`。
2. 清除 FFmpeg 解码器内部残留，且**不输出** flush 前的旧 PCM。
3. 只对新世代包解码。
4. 丢弃 `pts_us < target_us` 的同次 seek 目标前 PCM，直到到达目标位置。

`seek` 不得在 ApiLayer 线程直接调用 Backend 的 FFmpeg flush，因为 worker 可能正在访问
同一个解码器上下文。控制面只提交快照并唤醒 worker；FFmpeg 上下文始终由 worker 独占。

## EOF 与错误

Demuxer 发出 `DemuxerEndOfStream` 后，AudioDecoder 不能立即宣告结束：AAC 等 codec 可能
仍有内部延迟帧。worker 必须向 Backend 执行 drain（等价于向 FFmpeg 送空 packet 并持续
receive frame），将当前 generation 的剩余 PCM 全部写入 `AudioFrameStore`，然后发送：

```cpp
struct AudioDecoderEndOfStream {
    Generation::Value generation;
};

struct AudioDecoderError {
    AudioDecoderBackendError error;
};
```

`AudioDecoderEndOfStream` 由 AudioResampler 接收，以便它继续 drain 自己的 `SwrContext`。
错误也经 Notifier 上报；worker 回到可停止的等待状态，不在通知回调中执行恢复策略。

## FFmpeg 后端封装

沿用现有 `DefaultDemuxer + DemuxerBackend` 的分层：

```
DefaultAudioDecoder (domain worker)
  -> AudioDecoderBackend (contracts)
       -> FfmpegAudioDecoderBackend (infrastructure)
```

### `DefaultAudioDecoder` 的职责

- 管理 worker、cv、状态机、背压和 Notifier 订阅。
- 检查 generation，执行 seek 控制快照与 PTS 过滤。
- 将 Backend 返回的 `DecodedAudio` 包装成 `AudioFrame` 并推入 Store。
- 绝不包含 `AV*` 类型或调用 FFmpeg API。

### `AudioDecoderBackend` 的职责

接口接收 `AudioCodecConfig` 和 `EncodedPacket`，返回纯 `DecodedAudio`；最小能力为：

```cpp
configure(config)
decode(encoded_packet) -> zero or more DecodedAudio
drain() -> zero or more DecodedAudio
reset()
unconfigure()
```

Backend 不知道队列、generation、线程、Notifier、播放状态或输出设备。

### `FfmpegAudioDecoderBackend` 的职责

- 唯一持有 `AVCodecContext`、`AVPacket` 和 `AVFrame`。
- 基于 `AudioCodecConfig` 中的 codec 名称和 extradata 构造 `AVCodecContext`；无法找到
  decoder 或配置失败时返回结构化 Backend 错误。
- 从 `EncodedPacket` 的 payload 与微秒 PTS/DTS 重建输入 `AVPacket`；解码上下文的
  `pkt_timebase` 固定为微秒，避免向领域层泄漏 FFmpeg time base。
- 通过 `avcodec_send_packet/avcodec_receive_frame` 解码，并复制 AVFrame 数据为
  `DecodedAudio`，包括样本格式、planar 标记、采样率、声道数、每声道样本数与 PTS。
- `drain()` 送空 packet；`reset()` 调 `avcodec_flush_buffers()`；`unconfigure()` 释放所有
  FFmpeg 资源。

`FfmpegAudioDecoderBackend` 不得把 `AVFrame*` 借给 Store，也不得要求上层传入
`AVPacket*`。PCM 数据在 Backend 边界完成所有权转移，保证队列生命周期与 FFmpeg buffer
生命周期互不耦合。

## 测试范围

- `DefaultAudioDecoder`：configure/start/stop 幂等、输入空等待、输出满背压、stop 唤醒、
  generation 丢弃、seek PTS 过滤、EOF drain 与错误事件。
- `FfmpegAudioDecoderBackend`：真实媒体 fixture 的配置、解码 PCM 格式与 PTS、drain、
  无效 codec/extradata 的错误映射。
- 端到端：Demuxer -> AudioPacketQueue -> AudioDecoder -> AudioFrameStore，验证包与帧的
  generation、EOF 与关闭时线程收敛。

## 边界

- 不实现 `AudioFrameStore` 的具体无锁 SPSC 细节；该资源单独设计。
- 不实现 `swr_convert`、输出格式协商或 miniaudio；归 AudioResampler / AudioSink。
- 不实现设备音量、音频时钟或 Flutter 回调。
- 不在本阶段扩展真实 seek 的 DemuxerBackend 契约；Decoder 已定义其在定位成功后的响应。
