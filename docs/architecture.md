# SemiPlayer 模块架构设计

## Context

SemiPlayer 是一个 Rust 实现的跨平台播放器：FFmpeg 解封装/解码、cpal 播音频、最终编译为 C ABI 供 Flutter(Dart) 宿主调用。经多轮讨论，架构演进为：

- **单例全局**，对外接口无需 handle
- **时钟与音频播放都在 Rust**：cpal 回调里建立音频主时钟，视频同步线程读它
- **解耦管道**：demux→decode 用 cv+Mutex 有界队列；decode→cpal 用无锁 SPSC（`rtrb`）+ 生产方 sleep 背压 + 回调 try/静音
- **命令队列 + 句柄（对外控制模型）**：Dart 调用 → 往 ApiLayer 命令队列投递命令 → 立即返回句柄（UI 永不阻塞）。ApiLoop 单线程串行执行命令。句柄可 `await`（拿结果/确认）可 `cancel`。并发/节奏/时延由 Dart 业务侧自治，Rust 忠实串行执行 + 提供机制。控制信号走命令队列，不靠通知中心分发
- **去中心化状态**：不用上帝模块控制全局状态，每个模块自管状态
- **依赖注入（DI）**：IoCContainer 作为装配期装配器，按 DAG 拓扑顺序构造所有模块，构造时注入 `Arc<依赖>`。依赖图无真循环（资源/基础设施不反向依赖工作模块），故全部 Arc 注入，无需 Weak 破环
- **世代号（generation）机制**：seek/取消进行中的事务靠"数据标记识别"而非"时序协调"，消除主动清理和模块间顺序依赖，规避死锁

本阶段**只确定模块清单、职责、依赖关系**这张地基图。后续阶段再细化：(1) 每个模块的状态机与事件响应；(2) 对外 API；(3) 内部实现。

---

## 世代号机制（核心，贯穿全架构）

seek 这种跨管道事务，**真正的物理依赖只有两个**：定位只能在最上游做（Demuxer 持文件句柄）、flush 必须在新数据到达前。其余"必须按顺序清空各队列"的需求是**假依赖**，可被世代号消除。

### 机制

- 维护一个全局原子 `generation: AtomicU32`。
- 所有跨模块传递的数据（packet、视频帧、音频 PCM 块）**携带 generation 标记**。
- 消费者使用数据前**检查 generation**：等于当前则用，不等于则丢弃。

### 效果

| 旧思路（命令式协调） | 新思路（世代号识别） |
|------|------|
| 协调者命令队列"现在清空" | 队列不用清，旧数据消费时自动丢弃 |
| 必须在正确时刻清空 → 死锁风险 | 时刻无所谓，识别靠标记 → 无死锁 |
| 资源持有者要响应 seek、改逻辑 | **资源持有者逻辑零改动** |

> **世代号的职责边界**：只管"数据正确性"——保证 seek 后旧世代数据（解码器残留帧、队列里旧包）不与新数据混杂。它**不承担"取消进行中的 seek 命令"职责**——那是使用方用 cancel 做的。两者正交：世代号防数据混杂，cancel 防旧命令执行。

### 不变式（必须全局遵守）

> 所有跨模块数据带 generation；所有消费者消费前检查 generation。漏标或漏检任一处，机制失效。这是局部规则（每处加两行），不涉及跨模块协调。

### 世代号在 seek 中的职责

seek 涉及三层数据正确性，世代号只管其中一层：

- **第①层 解码器内部残留**（旧参考帧）→ decoder `avcodec_flush_buffers()`
- **第②层 跨 seek 旧数据**（上次播放残留在队列）→ **世代号**：数据带 generation，消费者丢弃旧世代
- **第③层 同次 seek 内目标前的帧**（FFmpeg 定位到最近关键帧的副作用）→ decoder PTS 过滤（`frame.pts < target` 丢弃）

世代号是**全局数据正确性机制**（原则），不是 seek 的编排逻辑。**generation+1 与 Demuxer 定位绑定**（定位完成后才 +1，保证对应新数据）。

→ **SeekCoordinator 模块砍掉**：seek 的具体编排（4 步顺序调用）是 ApiLayer 命令处理细节，见 `docs/modules/api_layer/seek.md`，不属总体架构。

---

## 命令与句柄机制（对外控制模型）

Dart 侧（UI 线程）与 Rust 的交互统一走"投递命令 + 句柄"，保证 UI 永不阻塞、控制权清晰。

### 基准原则

1. **命令模式**：所有 Dart 调用 → 往 ApiLayer 命令队列投递命令 → 立即返回句柄。UI 线程不阻塞。
2. **串行执行**：ApiLoop 单线程串行执行命令队列，一条一条来，天然无跨命令并发竞争。
3. **句柄能力**：每个句柄可 `await`（拿结果 / 错误 / 完成确认）+ 可 `cancel`。
4. **忠诚执行（哲学 A）**：播放器忠实执行队列里的每条命令，**不擅自跳过/合并**。所有命令一视同仁——进队列、忠实执行、完成才 resolve，无中间态。"聪明"的事（节流、合并、跳过旧 seek）由使用方用 cancel/await 自行控制。seek 是昂贵操作，使用方应清楚其成本并主动管理频率。
5. **取消语义**：队列里未开始的命令 → cancel 即移除不执行；已开始执行的命令 → 让它跑完（FFmpeg 操作不可打断），Dart 不等结果。**使用方用 cancel 实现"只保留最新 seek"等策略**，而非播放器内建覆盖。
6. **非法顺序检查**：命令执行前校验状态（未 open 就 play 等），非法则返回错误不执行。
7. **业务侧自治**：命令数量/节奏/时延由 Dart 业务侧自行控制，Rust 忠实执行不替它决策。

### 句柄设计

```
CommandHandle {
  id: u64,                 // 命令唯一 id
  cancel 标志/通道,         // Dart 调 handle.cancel() → 设标志
  done: oneshot/Future,    // Dart 可 await 拿结果(如 open 的 MediaInfo) 或 () 或错误
}
```

ApiLoop 执行每条命令前先检查 cancel 标志；执行完通过 done resolve 结果。

### 控制流

```
Dart 调用 ──▶ ApiLayer 投递 Command 到队列 ──▶ 立即返回 CommandHandle
ApiLoop 串行取 Command:
  - 检查 cancel → 取消则跳过
  - 检查状态合法性 → 非法则 done.resolve(Err)
  - 派发给对应模块执行(Demuxer/控制), 重活在工作线程
  - 完成 → done.resolve(结果/())

进度回调: ProgressReporter 独立线程读 AudioClock → 推 StreamSink(不走命令队列)
```

→ 控制信号走命令队列串行派发，进度走独立 Stream。状态通知（队列满/空、EOF、时钟跳点等）由 Notifier 通知中心承担，与控制命令分离、互不混入。

---

## 模块清单（25 个）

> **顶层是 `Player`（lifecycle 层）**，不属于模块清单——它管整个模块体系的生灭（`init` 装配所有模块、`shutdown` 逆序释放）。详见 `docs/lifecycle.md`。以下模块均由 `Player.init` 经 IoCContainer 装配。

### 🎛️ 基础设施层

| 模块 | 类型 | 职责 |
|------|------|------|
| **IoCContainer** | 装配器（无线程） | init 时按 DAG 拓扑顺序构造所有模块、构造时注入 `Arc<依赖>`；shutdown 时逆序释放。装配完成后持有各模块 Arc 供 ApiLayer 取用。纯装配，不提供运行时服务定位 |
| **CommandQueue** | 队列（无线程） | 接收 Dart 投递的 Command，供 ApiLoop 串行消费。每个 Command 关联一个 CommandHandle |
| **Notifier** | 通知中心（无线程） | 通用通知中心。模块注册感兴趣的通知类型（QueueNotFull/QueueNotEmpty/EOF/ClockJumped/Error 等），状态变化方发送通知。**取代队列自带 cv**：队列状态变（满→非满等）发通知，注册者被回调唤醒。承担**状态通知**职责（控制命令仍走 CommandQueue，不混入） |
| **Generation** | 原子标量（无线程） | `AtomicU32`，全局 seek 世代号。Demuxer seek 时 +1，所有数据携带、所有消费者检查 |

### 📦 资源管理者层（无线程，seek 逻辑零改动）

| 模块 | 持有 | 生产者 | 消费者 |
|------|------|--------|--------|
| **VideoPacketQueue** | 视频压缩包队列（每包带 generation） | Demuxer | VideoDecoder |
| **AudioPacketQueue** | 音频压缩包队列（每包带 generation） | Demuxer | AudioDecoder |
| **SubtitlePacketQueue** | 字幕压缩包队列（每包带 generation） | Demuxer | SubtitleDecoder |
| **VideoFrameStore** | 视频帧（硬解原生格式，GPU 句柄 + PTS + generation） | VideoDecoder | VideoRenderer |
| **AudioFrameStore** | 音频 PCM（无锁 SPSC `rtrb`，每块带 generation） | AudioDecoder | AudioResampler |
| **AudioResampledStore** | 重采样后音频 PCM（无锁 SPSC `rtrb`，cpal 目标格式，每块带 generation） | AudioResampler | AudioSink |
| **VideoRenderedStore** | 渲染好的视频帧（宿主格式 RGBA/BGRA，GPU 句柄或 CPU buffer + PTS + generation） | VideoRenderer | Compositor |
| **SubtitleFrameStore** | 渲染好的字幕位图（带 alpha 的 RGBA + 有效时间窗 + generation） | SubtitleRenderer | Compositor |
| **FinalFrameStore** | 合成后的最终画面（宿主格式 + PTS + generation） | Compositor | VideoSync |
| **AudioClock** | pts↔Instant 映射 | AudioSink（写） | VideoSync / ProgressReporter（读）；seek 时 ApiLayer 直接调 clock.jump_to 跳点 |

### ⚙️ 工作模块层（有线程，各自管状态）

| 模块 | 线程 | 职责 |
|------|------|------|
| **Demuxer** | 1 个 loop 线程 | 读文件 → 分流喂 Video/Audio/SubtitlePacketQueue；**seek 在此执行**：停旧读、av_seek_frame、generation+1、读新数据（clock.jump_to 由 ApiLayer 直接调，不经 Demuxer）|
| **VideoDecoder** | 1 个 loop 线程 | 取视频 packet → 查 generation 变化时自 flush → 硬解 → 喂 VideoFrameStore |
| **AudioDecoder** | 1 个 loop 线程 | 取音频 packet → 查 generation 变化时自 flush → 解码 → 喂 AudioFrameStore |
| **AudioResampler** | 1 个 loop 线程 | 取 AudioFrameStore（解码原始 PCM）→ `swr_convert` 转成 cpal 目标格式 → 喂 AudioResampledStore。**纯格式转换**，不解码不输出。seek 时 flush 内部残留 + gen 丢旧。变速不变调（set_speed）预留落点 |
| **SubtitleDecoder** | 1 个 loop 线程 | 取字幕 packet → 解析成字幕事件（SRT/ASS/PGS…）→ 维护当前 PTS 该显示的事件。**只解析+时间轴匹配，不出像素** |
| **VideoRenderer** | 1 个 loop 线程 | 从 VideoFrameStore 取硬解原生帧 → 格式转换（NV12 等 → 宿主 RGBA/BGRA）→ 喂 VideoRenderedStore。**纯转换，不碰字幕**。转换路径（GPU 直通 / copy-back）与图形上下文归属 TBD（见待确认项）|
| **SubtitleRenderer** | 1 个 loop 线程 | 字幕事件变化时用 libass 光栅化成带 alpha 的 RGBA 位图 → 喂 SubtitleFrameStore。**异步、只在事件变化时渲染**（缓存位图），避免拖慢合成 |
| **Compositor** | 1 个 loop 线程 | 从 VideoRenderedStore 取视频帧 + 从 SubtitleFrameStore 取（按 PTS 的）字幕位图 → 合成一张最终画面 → 喂 FinalFrameStore。**只合成，不转换不渲染**。依赖两个 rendered Store |
| **VideoSync** | 1 个 loop 线程 | 读 AudioClock → 从 FinalFrameStore 选帧（丢弃旧 generation 帧）→ 交付 Flutter。**回归纯粹末端消费者，不再驱动渲染/合成** |
| **AudioSink** | **复用 cpal 实时线程** | 取 AudioResampledStore（丢弃旧 generation）→ 送声卡 → 写 AudioClock |
| **ProgressReporter** | 1 个线程 | 100ms 读 AudioClock → 推 StreamSink |

### 🚪 接口层

| 模块 | 线程 | 职责 |
|------|------|------|
| **ApiLayer** | （Dart 调用线程） | 对外 init/open/play/pause/seek/close/get_state/set_volume/progress_stream/...。**只投递命令到 CommandQueue + 立即返回句柄**，不做重活、不阻塞 UI。维护 PlayerState 快照（仅投影，不控制） |
| **ApiLoop** | 1 个串行线程 | 从 CommandQueue 取命令 → 检查 cancel → 校验状态合法性 → 派发给对应模块执行 → resolve 句柄。重活触发后在工作线程跑，ApiLoop 负责"发起 + 等 done" |

---

## 依赖关系

依赖图是 DAG（资源/基础设施层不反向依赖工作模块），无真循环依赖。模块在**构造期**由 IoCContainer 注入 `Arc<依赖>`，运行时直接持有使用，不再查找容器。装配拓扑顺序：

```
第0层(无依赖, 先构造): Generation, CommandQueue, Notifier, 
        VideoPacketQueue, AudioPacketQueue, SubtitlePacketQueue,
        VideoFrameStore, AudioFrameStore, AudioResampledStore,
        VideoRenderedStore, SubtitleFrameStore, FinalFrameStore, AudioClock
第1层: Demuxer, VideoDecoder, AudioDecoder, AudioResampler, SubtitleDecoder   (注入第0层)
第2层: VideoRenderer, SubtitleRenderer   (注入第1层产物 Store + 第0层下游 Store)
第3层: Compositor   (注入 VideoRenderedStore + SubtitleFrameStore + FinalFrameStore)
第4层: VideoSync, AudioSink, ProgressReporter   (注入第0层; VideoSync 消费 FinalFrameStore, AudioSink 消费 AudioResampledStore)
第5层: ApiLoop   (注入第1-4层模块 + CommandQueue, 串行派发命令)
```

> 渲染/合成链比原设计多出两层（VideoRenderer/SubtitleRenderer 在第 2 层，Compositor 在第 3 层），故 VideoSync 下沉到第 4 层。原因：字幕与视频各自独立渲染成像素后，才由 Compositor 合成、VideoSync 选帧交付，存在"先渲染后合成再选帧"的串行数据依赖。

各资源队列**无 cv**——状态变化通过 Notifier 发送通知，注册者被回调唤醒。控制仍由 ApiLoop 串行派发命令直接调用模块方法。

| 模块 | 构造注入的依赖（Arc） | 控制来源 / 唤醒 |
|------|--------------|---------|
| Notifier | 无（被所有人依赖）| — |
| Demuxer | VideoPacketQueue, AudioPacketQueue, SubtitlePacketQueue, Generation, Notifier | ApiLoop 调 open()/start_reading()/seek()/stop_reading()/close()；阻塞时在自己的 cv 上等，Notifier 回调唤醒（QueueNotFull/Seek/Stop 等）|
| VideoDecoder | VideoPacketQueue, VideoFrameStore, Generation, FFmpeg 解码器, Notifier | ApiLoop 调 configure()/start()/stop()/seek()；查 generation 自 flush；Notifier 唤醒（QueueNotEmpty 等）|
| AudioDecoder | AudioPacketQueue, AudioFrameStore, Generation, FFmpeg 解码器, Notifier | 同 VideoDecoder |
| AudioResampler | AudioFrameStore, AudioResampledStore, Generation, Notifier | ApiLoop 调 configure()/start()/stop()/seek()；取 AudioFrameStore 经 swr_convert 转 cpal 格式喂 AudioResampledStore；查 generation 自 flush；Notifier 唤醒（FrameReady/NotFull 等）|
| SubtitleDecoder | SubtitlePacketQueue, Generation, Notifier | ApiLoop 调 start()/stop()/seek()；Notifier 唤醒（QueueNotEmpty 等）|
| VideoRenderer | VideoFrameStore, VideoRenderedStore, Generation, 图形上下文(TBD), Notifier | ApiLoop 调 configure()/start()/stop()/seek()；从 VideoFrameStore 取帧转换后喂 VideoRenderedStore；Notifier 唤醒（FrameReady 等）|
| SubtitleRenderer | SubtitleDecoder(查事件), SubtitleFrameStore, Generation, Notifier | ApiLoop 调 start()/stop()/seek()；事件变化时 libass 渲染位图喂 SubtitleFrameStore |
| Compositor | VideoRenderedStore, SubtitleFrameStore, FinalFrameStore, Generation, Notifier | ApiLoop 调 start()/stop()；从两个 rendered Store 取帧合成喂 FinalFrameStore；Notifier 唤醒（RenderedReady 等）|
| VideoSync | FinalFrameStore, AudioClock, Generation, Notifier | ApiLoop 调 start()/stop()/pause()；从 FinalFrameStore 选帧交付 Flutter；Notifier 唤醒（FinalReady/ClockJumped 等）|
| AudioSink | AudioResampledStore, AudioClock, Generation, cpal | ApiLoop 调 setup()/start_playback()/stop_playback()/set_volume()；复用 cpal 实时线程（cpal 回调驱动，不需 Notifier）|
| ProgressReporter | AudioClock, StreamSink | 独立线程，读 AudioClock 推进度 |
| CommandQueue | 无 | — |
| Generation | 无 | — |
| ApiLoop | CommandQueue + 各工作模块 Arc | — |
| IoCContainer | 无（持有所有人） | — |

> **队列无 cv，全靠 Notifier**：资源队列（PacketQueue/FrameStore）状态变化（满→非满、空→非空）时，通过 Notifier 发送通知；注册该通知的工作模块（生产者/消费者）被回调唤醒自己的 cv。这统一了状态通知机制，承担跨模块状态通知职责（控制命令仍走 CommandQueue，不混入）。各工作模块因此有自己的 cv，由 Notifier 回调唤醒。

### 数据流

```
文件 →[Demuxer]→ VideoPacketQueue(gen) →[VideoDecoder]→ VideoFrameStore(gen,硬解原生帧)
            │                                                            ↓
            │                                              [VideoRenderer](转换:NV12→RGBA/BGRA)
            │                                                            ↓
            │                                              VideoRenderedStore(gen) ──┐
            │                                                                        │
            └─→ SubtitlePacketQueue(gen) →[SubtitleDecoder]→ 字幕事件(PTS匹配)      │
                                              ↓                                      │
                                 [SubtitleRenderer](libass光栅化,仅变化时渲染)        │
                                              ↓                                      │
                                 SubtitleFrameStore(gen,带alpha位图+时间窗) ─────────┤
                                                                                    ▼
                                                                            [Compositor](按视频帧PTS取字幕位图,合成)
                                                                                    ↓
                                                                            FinalFrameStore(gen)
                                                                                    ↓
                                                                            [VideoSync](读AudioClock选帧)→ 纹理

            └─→ AudioPacketQueue(gen) →[AudioDecoder]→ AudioFrameStore(gen,解码原始PCM) →[AudioResampler]→ AudioResampledStore(gen,cpal格式) →[AudioSink]→ 声卡
       ↑seek:gen+1                                                              │
       │                                                                        ▼
    Generation                                                            AudioClock ←── seek: ApiLayer 调 jump_to
                                                                                  ▲
                                                                        VideoSync / ProgressReporter 读
```

> **字幕位图按 PTS 被动拉取**：Compositor 合成每帧时，按当前视频帧 PTS 从 SubtitleFrameStore 取有效时间窗内的字幕位图。字幕变化频率远低于视频帧率，SubtitleRenderer 仅在事件变化时渲染一次并缓存。
> **Compositor 依赖两个 rendered Store**：VideoRenderedStore（视频帧，已转换）+ SubtitleFrameStore（字幕位图，已渲染）。它只合成，不做转换/渲染。

### 控制流（命令串行派发）

```
Dart 调用 ──▶ ApiLayer 投递 Command ──▶ 返回 CommandHandle (UI 不阻塞)
ApiLoop 串行执行 (忠实执行, 不跳过/合并):
  play/pause → 调对应模块 set_paused(); 模块自洽(时钟冻结/cpal静音); 队列满背压自然停上游
  seek(pos)  → ApiLoop 顺序调各模块编排 (demuxer/视频decoder/音频decoder/字幕decoder/clock); 完成才 resolve
               数据正确性靠世代号(第②层)+flush(第①层)+PTS过滤(第③层); 详见 api_layer/seek.md
               字幕侧同走世代号自洽 flush (SubtitleDecoder/SubtitleRenderer 旧世代事件/位图被丢弃)
  open/close/shutdown → 同步探测/汇聚释放后 resolve (含启动/停止字幕线程)
```

> **字幕线程随 play 启动、随 close 停止**：SubtitleDecoder/SubtitleRenderer/VideoRenderer/Compositor 都是工作线程，play 时启动、close 时停止（和现有 decoder/sync 一致）。seek 时靠世代号让字幕侧自动丢弃旧事件/位图，无需专门协调。具体编排细节见下一阶段各模块文档。

---

## 关键设计原则

1. **去中心化状态**：每个工作/资源模块有自己的 `enum State`（如 `Running/Paused`），无全局上帝状态。ApiLayer 的 `PlayerState` 快照仅是给 Dart 查的投影，不参与控制。

2. **数据标记 > 时序协调**：跨模块数据正确性靠 generation 标记识别，不靠"在正确时刻清空"。这是规避死锁、实现"资源持有者零改动"、让取消 seek 免费的关键。

3. **命令串行 + 句柄**：控制走命令队列串行派发。Dart 投递命令拿句柄、UI 不阻塞、可 await 可 cancel。并发节奏由业务侧自治。

4. **依赖注入（DI）**：模块构造期由 IoCContainer 注入 `Arc<依赖>`，依赖关系写在 `new()` 签名里显式可见、可单测 mock。依赖图是 DAG 无真环，全部 Arc 注入，无 Weak、无运行时服务定位。

5. **seek 无专门协调者**：世代号消除假依赖后，seek 只剩 Demuxer 内部两三步 + AudioClock 跳 PTS，收进 Demuxer 自洽完成。无 SeekCoordinator，无握手死锁。

6. **AudioSink 特殊性**：复用 cpal 实时线程（不自建），遵守实时约束——零阻塞、try 取、空则静音。

---

## 范围边界（本阶段只做地基，不实现）

- ❌ 各模块状态机与事件响应的细节（下一阶段讨论）
- ❌ 对外 API 函数签名（待地基确认后）
- ❌ demux/decode/cpal/同步内部实现
- ❌ 平台纹理胶水层（硬解帧上 Flutter texture）
- ❌ FRB codegen + Dart 侧生成

---

## 待确认项

- [x] ~~对外控制模型~~ → 命令队列 + 句柄（投递即返回、可 await 可 cancel、串行执行、seek 破例）
- [x] ~~字幕/合成是否独立模块~~ → 独立。字幕侧拆为 SubtitleDecoder（解析+PTS匹配）+ SubtitleRenderer（libass光栅化）；视频侧拆出 VideoRenderer（格式转换）；末端 Compositor 只合成；各产出经 Store 解耦
- [x] ~~字幕位图如何传给合成~~ → Compositor 按视频帧 PTS 从 SubtitleFrameStore 被动拉取（SubtitleRenderer 仅事件变化时渲染缓存）
- [x] ~~显示流水线谁驱动~~ → 渲染/合成/选帧各自独立线程、靠 Store 解耦；VideoSync 回归纯粹末端消费者
- [x] ~~外挂字幕是否支持~~ → 本阶段不做，只支持 demuxer 解出的内嵌字幕流
- [ ] `get_state`：ApiLayer 维护 PlayerState 快照（仅投影不控制）——已倾向此方案，待认可
- [ ] ProgressReporter 是否保留为独立模块（还是合并进 VideoSync 顺带推进度）
- [ ] **VideoRenderer 转换路径**：GPU 直通（持有图形设备、shader 转换，保硬解收益但跨平台工作量大）vs copy-back（av_hwframe_transfer_data download + sws_scale 转 RGBA，简单但抵消部分硬解收益）。MVP 可先 copy-back，GPU 直通作未来优化
- [ ] **图形上下文归属**：VideoRenderer/Compositor 都需要图形设备（D3D11/ANGLE/Vulkan），暂未抽象 GpuContext 模块。依赖表中标 TBD，待决定是否抽基础设施层的 GpuContext 供两者共享注入（避免重复创建设备 + 跨设备传纹理）
- [ ] **SubtitleDecoder → SubtitleRenderer 的事件传递**：SubtitleDecoder 检测到该显示的事件变化时主动推、SubtitleRenderer 被唤醒才渲染（事件驱动，倾向此），还是 SubtitleRenderer 轮询查询
- [ ] 字幕轨选择/切换（多字幕轨、set_subtitle_track 命令）——本阶段暂不考虑，单轨
- [ ] 各 rendered Store（VideoRenderedStore/SubtitleFrameStore/FinalFrameStore）的内部实现：有界队列 vs 单帧快照、GPU 句柄池化
- [ ] 进入下一阶段：逐模块讨论状态机与命令响应（当前进行中：Demuxer）
