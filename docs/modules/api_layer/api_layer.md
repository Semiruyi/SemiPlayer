# ApiLayer 模块设计

> 对外接口层。Dart（Flutter 宿主）与 Rust 的唯一交互入口。
> 基准原则见 `docs/architecture.md` 的"命令与句柄机制"章节。

## Context

ApiLayer 是播放器对外的门面。基于已定稿的控制模型——**命令队列 + 句柄**：

- Dart 调用 → 投递命令进 CommandQueue → 立即返回 CommandHandle（**UI 永不阻塞**）。
- ApiLoop 单线程串行执行命令（无跨命令并发竞争）。
- 句柄可 `await`（拿结果/确认）可 `cancel`。
- **忠诚执行（哲学 A）**：播放器忠实执行每条命令，不擅自跳过/合并。所有命令一视同仁——完成才 resolve，无中间态。"聪明"（节流、跳过旧 seek）由使用方用 cancel/await 控制。
- seek 是昂贵操作（触发重新定位 + 解码），使用方应清楚其成本，主动用 cancel 管理频率（如拖动时 cancel 旧 seek）。
- 非法顺序检查：执行前校验状态，非法则 resolve(Err)。
- 业务侧自治：命令节奏/时延由 Dart 控制，Rust 忠实串行执行。

---

## 接口分类

> **注**：`init` / `shutdown` **不属于 ApiLayer**，它们是顶层 `Player`（lifecycle 层）的方法，管整个模块体系的生灭。详见 `docs/lifecycle.md`。ApiLayer 的接口仅在 init 之后、shutdown 之前可用。本文档以下接口均为 ApiLayer 接口。

ApiLayer 对外接口分三类，对应三种交互模式：

| 类别 | 接口 | 模式 | 句柄 |
|------|------|------|------|
| **控制命令** | open/play/pause/seek/close/set_volume/(set_speed预留) | 投递命令，返回 CommandHandle | ✅ 可 await/cancel |
| **状态查询** | get_state/get_duration | 同步直接返回快照（只读，不走队列） | ❌ |
| **事件流** | progress_stream | 返回 Stream，Dart 监听 | ❌ |

**关键区分**：查询类是纯读快照，**不进命令队列**、不返回句柄——否则连查个状态都要排队等前面的 seek，UI 会卡。只有**会改变状态的控制命令**才走命令队列。

---

## ApiLayer 的职责：命令路由 + 会话状态维护

ApiLayer 承担两个高度耦合的职责：

1. **命令路由/执行**：投递命令、串行执行、resolve 句柄（见命令与句柄机制）。
2. **播放会话状态维护**：记录会话状态，供 Dart 查询，并作为状态机判断命令合法性。

### 会话状态内容

- `PlayerState`：Idle / Ready / Playing / Paused / Ended / Error（生命周期状态机）
- 当前媒体信息：`src`、`duration`、`MediaInfo`（open 后才有）
- 会话级设置：音量、变速（set_volume/set_speed 设的）

### 为什么状态归 ApiLayer，不抽独立状态模块

- **权威写者就是 ApiLayer**：PlayerState 的变化只由命令执行触发（open→Ready、play→Playing、close→Idle），而命令执行就在 ApiLoop（ApiLayer）里。抽独立模块只会多一层无意义的间接。
- **合法性检查也在 ApiLayer**：命令执行前的非法顺序检查（未 open 就 play 等）需要读状态，与状态读取天然在一起。
- **独立模块会是贫血对象**：播放会话状态的方法只有 `get/set/check_legal`，全是读写自身字段、无独立逻辑；且使用者基本只有 ApiLayer（Dart 通过 ApiLayer 查询，不直接碰状态）。单一使用者 + 贫血逻辑 = 不该独立成模块。
- **对比该独立的模块**：资源管理者（PacketQueue/AudioClock）有独立逻辑且多使用者，故独立；会话状态不具备这些特征。

→ **不设独立的 PlayerState 模块**。状态字段内嵌在 ApiLayer。这也避免了模块清单膨胀（SeekCoordinator、EventBus 都经历过加了又砍）。

### 状态字段的线程可见性

`get_state()` 等查询由 **Dart 线程**同步调用（不走命令队列），而状态由 **ApiLoop 线程**修改。两线程访问同一状态字段，必须保证可见性：

- `PlayerState`、音量等用原子类型（`AtomicUsize` 编码枚举、`AtomicU32` 存音量定点数）或细粒度 `Mutex`。
- ApiLoop 写、get_state 读，原子读写、无锁快速。
- 复合字段（如 MediaInfo）用 `arc-swap` 原子整体替换指针，保证读到的快照一致。

---

## 接口清单

### 1. 生命周期控制命令（命令队列 + CommandHandle）

```
open(src: String)    -> CommandHandle<Result<MediaInfo>>
```
- 打开媒体源（文件路径或 URL）。探测文件、建 AVFormatContext、取流信息。
- 完成才 resolve：探测完拿 MediaInfo（含 duration/宽高/有无音视频流）。
- 若已有媒体打开，先内部 close 再 open。
- 成功后状态 → Ready。

```
close()              -> CommandHandle<Result<()>>
```
- **关闭当前媒体，回收其资源**（文件句柄、解码器、队列数据、停止工作线程）。
- 语义："这个媒体不放了，准备换下一个"。
- 完成才 resolve：所有相关线程停、资源释放完。
- 状态 → Idle。**播放器实例仍在**，可直接 open 新媒体（无需 init）。
- 对应 open 的逆操作。

> `init` / `shutdown` 不在此处，见 `docs/lifecycle.md`（顶层 Player 的方法）。

### 2. 播放控制命令（命令队列 + CommandHandle）

```
play()               -> CommandHandle<Result<()>>
```
- 开始/继续播放。状态 → Playing。
- 已 Playing 则无操作；Ended 则从开头重新播放。
- Pause 通过背压自然停上游（队列满 → Demuxer 自然阻塞）。

```
pause()              -> CommandHandle<Result<()>>
```
- 暂停。冻结音频时钟（带偏移修正，保证 resume 不跳）、cpal 切静音。
- 状态 → Paused。

```
seek(position_us: i64) -> CommandHandle<Result<()>>
```
- 精确 seek 到目标时间（微秒）。
- **昂贵操作**：触发 Demuxer 重新定位 + 解码器 flush + 管道重新填充。使用方应清楚成本。
- 与其他命令一样**忠实执行、完成才 resolve**（定位 + 世代号推进完，非等解码/播放跟上到屏幕）。
- **使用方控制频率**：拖进度条等高频场景，应主动 `cancel` 旧 seek handle 只保留最新，避免堆积。播放器不内建覆盖。
- 内部：Demuxer 执行 av_seek_frame + generation+1 + AudioClock 跳 PTS；其余模块靠世代号自洽 flush 旧世代数据，零协调。

```
set_volume(v: f32)   -> CommandHandle<Result<()>>
```
- 设置音量 [0.0, 1.0]，超出范围 clamp。影响 cpal 输出。

```
set_speed(s: f32)    -> CommandHandle<Result<()>>   // 预留，暂不实现
```
- 变速播放（如 0.5x/2x）。接口预留，后期实现（需音频重采样变速 + 视频跳帧策略）。

### 3. 状态查询（同步快照，不走队列）

```
get_state()  -> PlayerState
```
- 立即返回当前状态快照：`Uninitialized / Idle / Ready / Playing / Paused / Ended / Error`。
- 读 ApiLayer 维护的状态投影（不参与控制）。高频查询安全。

```
get_duration() -> i64
```
- 立即返回媒体总时长（微秒）。0 表示未知/直播流。
- 仅在 open 之后有意义；未 open 返回 0。

### 4. 事件流（不走队列）

```
progress_stream() -> Stream<Progress>
```
- 100ms 推一次 `{ position_us: i64, duration_us: i64 }`。
- ProgressReporter 独立线程读 AudioClock 推送，不经过命令队列。
- Paused 时 ProgressReporter 读时钟冻结状态，position 停在暂停点。

---

## CommandHandle 设计

统一泛型句柄，所有控制命令共用：

```
CommandHandle<T> {
    // T = MediaInfo (open), 或 () (其余命令)
    done: Future<Result<T>>,    // Dart 可 await 拿结果/错误/完成确认
    cancel(): void,             // 取消
}
```

### 取消语义

| 命令状态 | cancel 效果 |
|---------|------------|
| 队列里未开始 | 移除，不执行，done.resolve(Err(cancelled)) |
| 正在执行（任何命令） | 让它跑完（FFmpeg 操作不可打断），Dart 不等结果 |

> cancel 对所有命令语义统一，无特殊命令。使用方用 cancel（未开始的命令直接移除）实现"拖动只保留最新 seek"等策略。

### await 语义

- 有返回值的命令（open）：`await handle.done` 拿 `MediaInfo`。
- 无返回值的命令（play/pause/...）：`await handle.done` 拿 `()` 或错误，仅为确认"执行到了"。
- Dart 可选 await：不关心完成就丢掉句柄。

---

## 状态机（对外可见）

完整状态机跨越两层：lifecycle 层（`Uninitialized`，由 Player 管）+ ApiLayer 会话状态（`Idle/Ready/...`）。

```
  [lifecycle 层, Player 管]        [ApiLayer 会话状态]
                  
                 init()                
Uninitialized ──────────▶ Idle ──open()──▶ Ready ──play()──▶ Playing ⇄ Paused
     ▲                      ▲                                  │
     │                      │           close()                │ Ended(播完)
     │ shutdown()           └──────────────────────────────────┘
     │                         (任何会话状态 close → Idle)
     └──────────────────────────▶  shutdown()  (任何状态 → Uninitialized)
```

- `Uninitialized` ⇄ `Idle`：由 `init`/`shutdown`（Player，lifecycle 层）转换。
- `Idle`/`Ready`/`Playing`/`Paused`/`Ended`：ApiLayer 会话状态，由 open/play/pause/close 转换。
- shutdown 可从任何状态触发 → Uninitialized（见 lifecycle.md）。

状态：`Uninitialized / Idle / Ready / Playing / Paused / Ended / Error`

状态：`Uninitialized / Idle / Ready / Playing / Paused / Ended / Error`

---

## 命令执行流程（ApiLoop）

```
Dart 调用 ──▶ ApiLayer 构造 Command + CommandHandle ──▶ 投入 CommandQueue ──▶ 返回 handle
                                                                          (UI 不阻塞)

ApiLoop (单线程) 循环:
  1. 从 CommandQueue 取一条 Command
  2. 检查 handle.cancel 标志 → 已取消则跳过, resolve(Err(cancelled))
  3. 校验状态合法性 (非法顺序如未 open 就 play) → 非法则 resolve(Err(...))
  4. 派发给对应模块执行:
       open     → IoCContainer 装配 + Demuxer.probe() (工作线程), 等结果
       close    → 停所有工作线程 + 释放媒体资源, 汇聚确认
       shutdown → close 全部 + 释放 IoCContainer 所有模块, ApiLoop 退出
       play/pause → 调对应模块 set_paused()
       seek     → Demuxer.do_seek: av_seek + gen+1 + AudioClock跳PTS; 完成才 resolve
                   (使用方用 cancel 旧 handle 实现"只保留最新 seek")
       set_volume → 调 AudioSink.set_volume()
  5. 重活在工作线程跑, ApiLoop 负责"发起 + 等 done"
  6. done.resolve(结果/())  → 通知 await 的 Dart 侧
```

---

## 非法顺序检查（示例）

| 当前状态 | 调用 | 结果 |
|---------|------|------|
| Uninitialized | 任何除 init | Err("not initialized") |
| Idle | play/pause/seek/set_volume | Err("no media opened") |
| Playing | open | 先内部 close 再 open |
| Playing | play | 无操作，resolve(Ok(())) |
| Uninitialized | open/play/... | Err("not initialized")（init 在 lifecycle 层）|
| 任何 | shutdown | 合法（但属 lifecycle 层，见 lifecycle.md）|

---

## 范围边界

- ✅ 接口清单、CommandHandle、状态机、命令流程已定稿
- ❌ FRB 具体绑定代码（CommandHandle<T> 的 FRB 表达、cancel 信号机制）待实现阶段
- ❌ 各模块的 probe/start/stop/set_paused/do_seek 方法签名（逐模块设计时定）
- ❌ Dart 侧使用示例（待 FRB 生成后补）

---

## 待确认

- [ ] get_state 状态投影如何与各模块状态同步（open/play/pause 完成时 ApiLoop 更新投影）
- [ ] CommandHandle 在 FRB v2 下的精确绑定形态（StreamSink? 自定义对象? cancel 信号?）
