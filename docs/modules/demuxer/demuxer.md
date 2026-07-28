# Demuxer 模块设计

## 当前实现范围

`Demuxer` 是 ApiLayer 使用的解封装模块。`DefaultDemuxer` 通过
`DemuxerBackend` 打开、探测和读取媒体，并把当前选中音频流的包按顺序写入
`AudioPacketSink`。上层不接触 FFmpeg 的 `AV*` 类型。

当前实现：

- `open`：调用 backend 探测媒体，选择第一个视频、音频和字幕流，成功后推进共享 `Generation`。
- `start`：仅在 `Ready` 状态创建 worker；已经 `Reading` 时幂等成功。
- `stop`：停止并 join worker，终止当前媒体会话；尚未成功入队的 pending item 按设计丢弃。
- `close`：停止 worker、关闭 backend 媒体资源并回到 `Closed`，不清空共享队列。
- `seek`：当前只记录目标位置；backend 定位和 generation 推进尚未实现。
- worker 只生产音频包；视频、字幕包当前跳过。

## 依赖

| 依赖 | 用途 |
|------|------|
| `DemuxerBackend` | 打开媒体、探测流、读取 backend 包 |
| `AudioPacketSink` | 接收有序的 `AudioPacket` 和 `AudioPacketEndOfInput` |
| `Generation` | 标记媒体会话；成功 `open` 后推进一次 |
| `Notifier` | 接收 `AudioQueueNotFull` 背压唤醒；发送 `DemuxerReadError` |

worker 创建时把当前 generation 和音频流 id 固定到 `WorkerSession`，本次会话产生的
所有数据项都使用这个 generation。`close/open` 后，旧队列项由下游 generation 检查丢弃。

## 状态机

```
Closed ──open 成功──▶ Ready ──start──▶ Reading
  ▲                    │                 │
  │                    │ stop            ├─EOF + EndOfInput 入队──▶ Exhausted
  │                    ▼                 └─读包错误──────────────▶ Failed
  │                  Stopped
  │                    ▲
  └─close── Stopped ◀── worker 退出 ◀── Stopping ◀── stop（Reading）

Exhausted / Failed ──close──▶ Closed
```

| 状态 | 含义 | `start()` |
|------|------|-----------|
| `Closed` | 没有打开的媒体资源 | 失败，必须先 `open()` |
| `Ready` | 媒体已打开并完成探测，尚未读包 | 创建 worker 并开始读包 |
| `Reading` | worker 正在从 backend 读包 | 幂等成功 |
| `Stopping` | 收到 `stop()`，等待 worker 退出 | 失败 |
| `Stopped` | 当前媒体会话被 stop 终止，worker 已退出 | 失败，必须 `close()` 后重新 `open()` |
| `Exhausted` | backend 已 EOF，结束项已经入队，worker 已退出 | 失败，必须重新 `open()` |
| `Failed` | backend 读包失败，worker 已退出 | 失败，必须重新 `open()` |

### stop 的语义

`stop()` 是当前媒体会话的终止操作，不是可恢复的 pause：

1. `Reading` 状态进入 `Stopping`，唤醒 worker 并等待其退出。
2. worker 的 `WorkerSession` 随线程结束；其中尚未成功入队的 pending item 被丢弃。
3. worker 退出后状态进入 `Stopped`。
4. `Stopped` 状态不能直接 `start()`；必须先 `close()`，再 `open()` 建立新媒体会话。

`Ready` 状态调用 `stop()` 时没有 worker，会直接进入 `Stopped`。对
`Exhausted`、`Failed` 或 `Stopped` 重复调用 `stop()` 是安全的幂等操作。

`stop()` 不清空共享队列。队列中已经入队的旧包和旧的
`AudioPacketEndOfInput` 仍保留在 FIFO 中，由消费者检查 generation 后丢弃。

## 对外接口

| 方法 | 约束 | 职责 |
|------|------|------|
| `open(src)` | 仅 `Closed` | 打开并探测媒体，选择默认流，推进 generation，进入 `Ready` |
| `start()` | `Ready` 或 `Reading` | 创建 worker 并开始读包；`Reading` 下幂等 |
| `stop()` | 任意已打开状态 | 结束当前会话并 join worker；pending item 可丢弃 |
| `seek(pos)` | 非 `Closed` | 当前仅记录目标位置，未执行 backend 定位 |
| `close()` | 任意状态 | 确保 worker 退出，关闭 backend，回到 `Closed` |

成功 `open()` 不启动 worker。首个成功媒体会话使用 generation `1`，每次后续成功
`open()` 继续推进 generation。`open()` 失败时保持 `Closed`。

## 工作线程流程

```
start
  └─创建 WorkerSession(generation, audio_stream_id)
       └─等待工作
           ├─Stopping       → 退出，丢弃 pending item
           ├─有 pending item → try_push，满则继续等待 AudioQueueNotFull
           └─否则           → backend.read_packet()
                              ├─非选中音频流 → 跳过
                              ├─音频包       → 标记 generation 后 try_push
                              ├─队列满       → 保存为 pending item
                              ├─EOF          → 入队 AudioPacketEndOfInput
                              └─错误         → 发送 DemuxerReadError 并进入 Failed
```

普通音频包和 `AudioPacketEndOfInput` 共用队列容量和背压。结束项只有在成功入队后
worker 才进入 `Exhausted`；因此结束项不会绕过 FIFO，也不会绕过队列满状态。

`AudioPacketEndOfInput` 不是 Notifier 事件。消费者取出队列项后，必须先检查其
generation；只有当前 generation 的结束项才可以触发 decoder drain。读包错误才通过
`DemuxerReadError` 通知 ApiLayer。

## 并发与生命周期

- `open/start/stop/seek/close` 使用同一把 mutex 串行化状态转换。
- worker 不持有 demuxer mutex 调用 backend 的 `read_packet()`，避免把 backend I/O
  放在状态锁内。
- `stop()` 和 `close()` 会 join worker，确保 backend 在 worker 退出后才关闭。
- `AudioQueueNotFull` 回调只设置 wake-up hint 并通知 demuxer cv，不执行读包或队列操作。
- 当前 backend 的 `read_packet()` 若阻塞，`stop()` 会等待它返回；可取消的实时/网络输入
  需要后续在 backend 契约中增加中断能力。

## 后续实现

- 为 `DemuxerBackend` 增加 seek 契约，实现真实定位和定位后的 generation 推进。
- 生产视频、字幕包，并分别接入对应队列。
- 由 ApiLayer 编排 decoder、resampler、clock 等模块的 seek 和播放结束流程。
