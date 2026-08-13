# Demuxer 模块设计

## 定位

`Demuxer` 负责打开媒体、探测流并持续生产压缩包。它是一个自持有 worker
线程的工作模块：构造时启动线程，析构时停止线程并等待退出。

Demuxer 只向下游写入受控的值类型包，不暴露 FFmpeg 的 `AV*` 类型。当前实现
只生产选中的音频流，视频和字幕流的生产接口保留给后续扩展。

```text
DemuxerBackend -> DefaultDemuxer(worker) -> AudioPacketQueue
                         |
                     Generation
```

## 对外契约

```cpp
class Demuxer {
public:
    virtual ~Demuxer() = default;

    virtual std::expected<DemuxerOpenResult, DemuxerError>
    open(std::string_view source) = 0;

    virtual std::expected<void, DemuxerError>
    seek(std::int64_t position_us, SeekMode mode) = 0;

    virtual void close() noexcept = 0;
};
```

### 同步语义

- `open()`、`seek()` 是同步阻塞操作，返回时对应操作已经在 worker 中完成。
- `close()` 是同步阻塞操作，返回时当前媒体会话已经停止且 backend 已关闭。
- 不提供 `start()`、`stop()` 或 `pause()`。`open()` 成功后自动开始生产；下游停止消费
  后，队列背压会使 worker 自然等待。
- 对外控制调用不允许并发。调用方必须串行调用 `open/seek/close`；实现内部仍通过
  命令队列保证 backend 只被 worker 线程访问。

### 方法约束

| 方法 | 前置状态 | 完成后的状态 | 失败行为 |
|------|----------|--------------|----------|
| `open(source)` | `Closed` | `Running` | 保持 `Closed`，必要时关闭 backend |
| `seek(position, mode)` | `Running` 或 `Exhausted` | `Running` | 返回结构化错误，不修改当前会话 |
| `close()` | 任意会话状态 | `Closed` | `noexcept`，尽力完成清理 |

默认不支持在已有媒体上直接 `open()` 替换。调用方应先 `close()`，再 `open()`。

## Worker 生命周期

```text
构造函数
  └─创建 worker，进入 Closed 等待

open(source)
  └─OpenCommand ──同步等待──> worker.open/probe/select
                              └─Running，开始 read_packet

seek(position, mode)
  └─SeekCommand ──同步等待──> worker 按前一/后一关键帧定位，继续 Running

close()
  └─CloseCommand ──同步等待──> 停止当前会话并 backend.close，回到 Closed

析构函数
  └─ShutdownCommand ──join──> worker 退出
```

worker 线程只在析构时退出。`close()` 不销毁线程，只结束当前媒体会话。

## 状态机

```text
                 open 成功
Closed ─────────────────────────▶ Running
  ▲                                  │
  │                                  ├─EOF + EndOfInput 入队──▶ Exhausted
  │                                  └─backend 错误──────────▶ Failed
  │                                      │       │
  └──────────── close ───────────────────┴───────┘

Running/Exhausted ──seek──▶ Running
```

`Opening`、`Closing` 和 `ShuttingDown` 是 worker 内部控制状态，不作为对外可观察状态
返回。控制命令按顺序执行，后提交的命令不会越过前一个命令。

## 内部命令

worker 通过条件变量等待以下命令：

```text
Open(source, promise<DemuxerOpenResult>)
Seek(position_us, promise<void>)
Close(promise<void>)
Shutdown(promise<void>)
```

每个同步接口创建一个命令并等待对应 promise。命令执行期间，backend 不被其他线程
调用。worker 在没有控制命令且会话为 `Running` 时执行数据面循环。这里的 pending
不是 backend 读包缓存，而是“已经从 backend 结果转换出来、等待提交给
`AudioPacketQueue` 的音频输出项”，当前只可能是 `AudioPacket` 或
`AudioPacketEndOfInput`：

```text
pending audio output 优先
  ├─队列可写 -> try_push
  └─队列已满 -> 等待 AudioQueueNotFull

无 pending item
  └─backend.read_packet()
       ├─非选中流 -> 丢弃并继续
       ├─音频包 -> 标记 generation 后写入队列
       ├─EOF -> 写入 AudioPacketEndOfInput，进入 Exhausted
       └─错误 -> 发送 DemuxerReadError，进入 Failed
```

`AudioPacketEndOfInput` 与普通包共用队列容量，必须成功入队后才能进入 `Exhausted`。
`close()` 和未来 `seek()` 可以丢弃尚未提交给下游的 pending audio output；已经入队的
旧数据继续由 generation-only 机制处理。

## Seek 与 Generation

`seek()` 的实际 backend 定位和 generation 更新在 worker 中完成：

1. worker 停止当前读取并清理尚未入队的 pending audio output；
2. 调用 backend 的 reset/seek 操作；
3. 推进共享 `Generation`；
4. 创建新的 `WorkerSession`，继续向队列生产新世代数据。

下游通过 generation 丢弃 seek 前已经排队的旧包。seek 不在调用方线程直接访问 backend。

## 错误与关闭

- backend 错误通过 `DemuxerError` 同步返回给当前控制调用；数据面读取错误同时发送
  `DemuxerReadError` 通知。
- `Failed` 状态不自动重试。调用方应 `close()` 后重新 `open()`。
- `close()` 必须先停止数据面循环，再调用 backend.close()，避免 backend 与 worker 并发访问。
- 当前 backend 若正在 `read_packet()` 阻塞，`close()` 和析构会等待它返回；可中断读取
  能力留待后续 backend 契约扩展。

## 依赖

| 依赖 | 用途 |
|------|------|
| `DemuxerBackend` | 打开媒体、探测流、读取包、seek/reset 和关闭 |
| `AudioPacketSink` | 接收有序 `AudioPacket` 与 `AudioPacketEndOfInput` |
| `Generation` | 标记媒体会话和 seek 世代 |
| `Notifier` | 接收队列非满通知，发送读取错误通知 |

所有依赖在构造期注入。Demuxer 不通过 IoC 做运行时服务定位。

## 测试范围

- 构造时 worker 启动，析构时 worker 完整退出；
- `open/seek/close` 的同步完成语义；
- 重复 close、非法状态调用和 backend 错误；
- 队列背压下 pending item 的顺序与恢复；
- EOF 结束项的顺序和 generation；
- seek 后旧世代包被丢弃；
- backend 阻塞读取期间 close 的等待语义。
