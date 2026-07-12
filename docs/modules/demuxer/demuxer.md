# Demuxer 模块设计

> 解封装模块。管道最上游，唯一持文件句柄、唯一懂"流"概念的模块。
> 对外接口由 ApiLayer 调用（见 `docs/modules/api_layer/` 各命令编排）。本文件描述其内部设计。

## 定位

```
文件 →[Demuxer]→ VideoPacketQueue(gen) →[VideoDecoder]→ ...
            └─→ AudioPacketQueue(gen) →[AudioDecoder]→ ...
```

- **唯一持 AVFormatContext**（文件句柄）。
- **唯一懂流概念**：探测流信息、产出建解码器配置（video_config/audio_config）、包分流。流级别知识集中于此，不泄漏给下游。
- **不依赖 AudioClock**（seek 跳点 clock.jump_to 由 ApiLayer 直接调）。
- **不依赖任何 Decoder**（decoder 靠世代号自洽 flush，不需 demuxer 通知）。

---

## 依赖

### 构造期注入（DI，Arc 持有）

| 依赖 | 用途 |
|------|------|
| `VideoPacketQueue` | 推视频包 |
| `AudioPacketQueue` | 推音频包 |
| `Generation` | seek 时 +1；读包时给包打标记 |
| `Notifier` | 注册 QueueNotFull 通知（队列满时被唤醒）；发送 EOF / Error 通知 |

### 内部状态（非注入，自己持有）

| 内部状态 | 何时有 |
|---------|--------|
| `AVFormatContext*`（文件句柄）| open 后有，close 释放 |
| 视频/音频流索引 | open 探测后确定 |
| 线程状态（见状态机）| 运行时 |
| `seek_intent: Mutex<Option<(pos, oneshot::Sender<()>)>>` | seek 命令设置，工作线程消费 |
| 自有 `cv`（+ Mutex）| 工作线程的统一唤醒点 |

---

## 状态机

```
Constructed ─open()─▶ Idle ─start_reading()─▶ Reading ⇄ Seeking
                        ▲                          │
                        │   EOF (发 EOF 通知, 线程 wait 在 Idle, 不退出)
                        │
                        └─stop_reading()/close()─▶ Stopping ─▶ Stopped (线程退出)
```

| 状态 | 含义 | 线程 |
|------|------|------|
| **Constructed** | init 装配完，空壳，无文件 | 未起 |
| **Idle** | open 探测完，文件已开、流信息已知，未在读；或 EOF 后 | 常驻 wait（方案 Y）|
| **Reading** | play 启动后，线程读包循环 | 在跑 |
| **Seeking** | 收到 seek，正在定位（短暂过渡）| 在跑 |
| **Stopping** | 收到 stop/close，线程准备退出 | 在跑（即将退出）|
| **Stopped** | 线程已退出 | 已退出 |

**线程生命周期（方案 Y，常驻）**：
- **首次 start_reading 时 spawn**（不播放不占线程；按需起）。
- 线程常驻到 **close/shutdown**（Stopping）才退出。
- pause / EOF / 队列满 → 线程 **wait 在 cv**，不退出。
- close/shutdown 后线程没了，下次 start_reading 检查并重新 spawn（天然处理"close 后再 open"）。

---

## 对外接口

| 方法 | 调用时机 | 职责 |
|------|---------|------|
| `open(src) → MediaInfo + 流配置` | open 命令 | 探测文件，返回 MediaInfo + video_config/audio_config |
| `start_reading(start_pts)` | play 命令（冷启动）| 首次 spawn 线程；若 start_pts≠0 先定位+gen+1；state=Reading |
| `seek(pos)` | seek 命令 | 设 SeekIntent + 唤醒 + oneshot 等定位完成 |
| `stop_reading()` | close 命令 | state=Stopping + 唤醒，等线程退出（join）|
| `close()` | close 命令 | 确保 stop_reading；释放 AVFormatContext；state=Constructed |

### 接口细节

#### `open(src)`
- 在 ApiLoop 线程调用（不在工作线程）。
- `avformat_open_input` + `avformat_find_stream_info`。
- 找出视频流/音频流索引。
- 构造 MediaInfo（duration/宽高/流标志）+ video_config/audio_config（含 extradata/codecpar，纯数据，给 decoder configure）。
- state = Idle。**不启动工作线程**（start_reading 的事）。

#### `start_reading(start_pts)`
- **首次调用 spawn 工作线程**（检查"线程是否在跑"，按需 spawn，原子防重复）。
- 若 `start_pts != 0`：av_seek_frame(start_pts) + generation+1（Ready 态 seek 调起点的落地）。
- state = Reading。
- notify cv 唤醒工作线程进入读循环。

#### `seek(pos)` — oneshot 同步等待
```
seek(pos):
    let (tx, rx) = oneshot::channel()
    *seek_intent.lock() = Some((pos, tx))    // 存意图 + 完成信号发送端
    cv.notify_one()                           // 唤醒工作线程(若在队列满wait上)
    rx.recv()                                 // 等工作线程处理完定位
    // 返回 (定位已完成)
```
- 符合 seek 命令"完成才 resolve"。
- oneshot 天然防 lost wakeup（send 早于 recv 也保留）。
- **连续 seek**（前一个未处理）：新 seek 覆盖 seek_intent，旧 sender drop → 旧 seek 的 `rx.recv()` 得 `Err(sender dropped)` → 旧 seek 返回"被取消"。实践中 ApiLayer 串行 + Dart 侧 cancel 旧 handle，少触发，但内部 robust 处理。

#### `stop_reading()`
- state = Stopping。
- cv.notify_one()（唤醒阻塞中的工作线程）。
- **等线程退出**（join / 等 Stopped 信号）。
- close 命令的第③步，汇聚线程退出。

#### `close()`
- 确保 stop_reading 已调（线程已 Stopped）。
- `avformat_close_input`，释放 AVFormatContext。
- state = Constructed（回空壳，可再 open）。

---

## 工作线程循环（核心）

线程常驻，**所有唤醒都回到循环顶统一检查**，有事干活，仅 Stopping 退出。

**设计精髓**：
- **循环顶是唯一的调度点**——所有 cv.wait 都在循环顶附近，醒来后重新检查 state + 队列，不判断"被谁唤醒"。
- **push 不 wait**——push（try_push）只管"能放就放、满了就报告满"，wait 由循环顶统一处理。
- **队列满 → continue 回循环顶**，让 seek/stop 能插队（不会"队列刚不满、seek 来了却没处理"）。
- **while 防虚假唤醒**——cv.wait 永远在 while 里，每次醒来重新验证条件。

```
fn run_loop():
    loop:                                              # ← 循环顶, 所有唤醒回到这里
        let g = state_mutex.lock()
        
        # 等待: 直到"该干活" (while 防虚假唤醒)
        while not 该干活(g):
            g = cv.wait(g)                             # 醒来重新检查, 不判断唤醒来源
        
        match state:
            Stopping =>
                state = Stopped; drop(g); return       # 唯一退出点
            
            SeekIntent(pos, tx) =>
                state = Seeking; drop(g)
                av_seek_frame(pos)
                generation.fetch_add(1)                # 定位后+1, 与读新数据绑定
                clear seek_intent
                let _ = tx.send(())                    # 告诉 seek(): 定位完成
                state = Reading
                continue                               # 回顶读新
            
            Reading =>
                drop(g)                                # 读包不该持锁
                pkt = av_read_frame()                  # 可能短暂阻塞(不特殊处理, 见下)
                match pkt:
                    EOF =>
                        notifier.send(EOF)            # → ApiLayer 设 player_state=Ended
                        state = Idle                   # 回 Idle, 线程 wait(不退出)
                        continue                       # 回顶 wait
                    Error(e) =>
                        notifier.send(Error(e))
                        state = Idle; continue
                    Ok(pkt) =>
                        pkt.generation = generation.load()
                        queue = 分流(视频→VideoPacketQueue / 音频→AudioPacketQueue)
                        if try_push(queue, pkt):       # 放成功
                            continue                   # 读下一个包
                        else:                          # 队列满, 没放进去
                            continue                   # ← 回循环顶! 循环顶的 while 会 wait
```

### 该干活(g) 的定义

循环顶 `while not 该干活(g)` 的判断条件——满足任一就该醒来干活：
- `state == Stopping`（要退出）
- 有 `SeekIntent` 待处理（要定位）
- `state == Reading` 且 **目标队列不满**（可以 push 了）

否则（Idle、或 Reading 但队列满、或 pause）→ 继续 wait。

> **队列满如何 wait**：Reading 态 push 失败 → `continue` 回循环顶 → 循环顶 `while not 该干活` 检查发现"队列满=不该干活"→ 进入 wait。被 Notifier(QueueNotFull) 唤醒后 → while 重新检查 → 队列不满了 → 退出 while → 重新走 match → Reading → 读包 → try_push。**seek/stop 在此期间若到来，循环顶的 match 会优先处理**（SeekIntent/Stopping 分支）。

### try_push（不 wait，只放/报告满）

```
try_push(queue, pkt) -> bool:        # 返回是否成功放入
    if queue.is_full():
        return false                  # 满, 告诉调用方"没放", 由循环顶决定 wait
    queue.push(pkt)
    notifier.send(QueueNotEmpty)      # 通知 decoder 可取包
    return true
```

**try_push 纯粹**：不 wait、不处理 seek/stop、不持 state 锁。只判断满不满、放数据、发通知。所有控制流和 wait 都在循环顶。

### 为什么 push 满了是 continue 回循环顶（而不是在 push 里 wait）

- **职责单一**：try_push 只管数据（放/报告满），seek/stop/wait 集中在循环顶。
- **让 seek/stop 插队**：push 满回循环顶 → 循环顶优先检查 SeekIntent/Stopping → seek/stop 先处理，不会"傻等队列不满而忽略 seek"。
- **wait 集中**：所有 cv.wait 在循环顶一处，配合 while 防虚假唤醒，不分散在 push 里。
- **不判断唤醒来源**：醒来回循环顶看现状（state + 队列），不问"谁叫醒我"——cv 的正确用法。

> 注意：push 满了 continue 时，**当前读出的 pkt 不能丢**——它没放进队列。所以实际实现里 pkt 要保留（循环顶下次 try_push 同一个 pkt），或重新 av_read_frame。倾向保留 pkt（循环顶下次再 try_push 它），避免重复读包。

---

## Notifier 注册与发送

### 注册（被唤醒）
- **QueueNotFull（VideoPacketQueue / AudioPacketQueue）**：队列从满变非满时，Notifier 回调唤醒 demuxer 的 cv。工作线程醒来后**回到循环顶**重新检查（不判断被谁唤醒），发现队列不满 + state=Reading → 继续 try_push。
  - 回调在**发送方线程**（decoder pop 包的线程）执行——必须极轻量（仅 `cv.notify_one()`），不拿重锁、不干重活。

### 发送（通知别人）
- **EOF**：读到文件结尾 → send(EOF) → ApiLayer 注册，设 player_state=Ended。
- **Error**：解封装出错 → send(Error) → ApiLayer 处理。
- **QueueNotEmpty**：push 包后队列非空 → send → decoder 注册，唤醒它取包。

### 自有 cv 的唤醒来源（统一唤醒点）
demuxer 的 cv 可被多方 notify，醒来在循环顶统一检查：
- Notifier 回调（QueueNotFull）→ notify cv。
- ApiLoop 调 seek（oneshot tx + notify cv）。
- ApiLoop 调 stop_reading（state=Stopping + notify cv）。

---

## 关键设计决策

### 工作线程常驻（方案 Y）
线程首次 start_reading 时 spawn，常驻到 close/shutdown。pause/EOF/队列满都 wait 在 cv（不退出）。**理由**（性能+复杂度双优）：
- 无反复 spawn 开销；pause/resume、EOF 再 seek 响应快。
- 上下文（AVFormatContext）在线程内连续。
- 首次 start_reading spawn（非 init spawn）：不播放不占线程；spawn 逻辑集中一处，天然处理 close 后再 open。
- close 必须显式 state=Stopping + notify 才能让线程退出（默认不退出）。

### seek 用 oneshot channel 同步等待
oneshot 天然是"请求-响应"语义，防 lost wakeup。seek 设 SeekIntent+sender、notify、等 rx。连续 seek 旧 sender drop → 旧 seek 收 Err（取消），与 cancel 语义一致。

### generation+1 与定位绑定
SeekIntent 分支内：av_seek_frame **之后**、读新数据**之前** +1。保证 generation 永远对应定位后的新数据（不会把定位过程中读到的旧包标成新世代）。

### EOF 后回 Idle，线程不退出
EOF 发通知后 state=Idle，线程 wait（方案 Y）。可再 seek（调起点重播）或 close。ApiLayer 收 EOF 设 player_state=Ended。

### av_read_frame 阻塞不特殊处理
seek/stop 时若工作线程正阻塞在 av_read_frame（读文件 I/O），接受"读完当前包回循环顶才响应"的短暂延迟。本地文件 av_read_frame 极快（<1ms），无感。不引入自定义 I/O 中断的复杂度。

### 不依赖 AudioClock / Decoder
- clock.jump_to 由 ApiLayer 直接调（demuxer 不碰 clock）。
- decoder 靠世代号自洽 flush（demuxer 不通知 decoder）。
- 保持 demuxer 依赖最小（Video/AudioPacketQueue + Generation + Notifier），DAG 干净。

---

## 坑与边界

### Notifier 回调线程上下文
QueueNotFull 回调在 decoder 线程执行。回调**仅 notify cv**，绝不拿 demuxer 的重锁或干重活，否则可能和 demuxer 工作线程死锁或拖慢 decoder。

### oneshot 连续 seek
连续 seek 时旧 sender 被 drop，旧 seek 的 rx 收 Err。内部要处理 Err（返回取消），不能 panic。

### push 满时的状态穿插
push 队列满时 wait，但 wait 期间 seek/stop 可能到来。所以 wait 醒来不能直接继续 push，要**回循环顶重新检查 state**（统一处理 seek/stop/读）。避免"seek 来了还在傻推旧包"。

### 线程 spawn 的原子性
start_reading 的"检查线程是否在跑 + spawn"要原子（标志位 / once 逻辑），防重复 spawn。

### close 时序
close 必须先 stop_reading（线程 Stopped）再释放 AVFormatContext，否则工作线程可能访问已释放的 context。stop_reading 的 join 保证线程退出后才 close。

---

## 边界（本文档不涉及）

- ❌ FFmpeg API 的具体调用细节（avformat_open_input 参数等）→ 实现阶段
- ❌ PacketQueue 的内部实现（无 cv + Notifier 协作）→ packet_queue.md（待设计）
- ❌ video_config/audio_config 的具体字段 → 实现阶段 / decoder 文档
- ❌ ApiLayer 如何注册 EOF/Error 通知 → api_layer.md（待补）
