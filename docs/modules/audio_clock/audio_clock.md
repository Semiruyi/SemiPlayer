# AudioClock 模块设计

> 音频主时钟。整个播放器的"时间感"基准——pts ↔ wall clock(Instant)的双向映射。
> 对外接口由 ApiLayer 调用（seek 的 `jump_to`、open/close 的 `reset`）；被 AudioSink 写、VideoSync/ProgressReporter 读。本文件描述其内部设计。

## Context

播放器需要一个稳定的"当前播放到哪个时间点(pts)"的来源,供 VideoSync 选帧、ProgressReporter 上报进度、seek 跳点后对齐。SemiPlayer 采用**音频主时钟**:声卡以恒定采样率消耗样本,**已播放样本数 = 精确流逝的音频时间**,是整个系统最稳的时间基准(不依赖系统时钟漂移、不受 CPU 抖动影响)。

AudioClock 就是这个基准。它维护"某个 wall-clock 时刻对应某个音频 pts"的映射,所有想查当前 pts 的模块用当前系统时间推算。

---

## 定位

```
              写(校准基准)                读(推算当前 pts)
AudioSink(miniaudio 实时线程) ──→ AudioClock ←── VideoSync / ProgressReporter
                                    ↑
                            ApiLoop 控制: jump_to / reset / freeze / unfreeze
```

- **资源管理者层,无线程**(被多线程读写)。
- DAG 第 0 层,无依赖,被所有人依赖。
- **不做**:不决定播什么(VideoSync 选帧)、不驱动音频播放(miniaudio)、不参与世代号(它是标量不是数据流)。

---

## 职责

- **建立 pts ↔ wall clock 映射**:记录"某 wall-clock 时刻 `base_time` 对应音频 pts `base_pts`"。任意时刻当前 pts = `base_pts + (now - base_time) × rate`。
- **被 AudioSink 校准**:miniaudio 回调喂声卡数据后,用样本计数算出 ground truth 当前 pts,刷新基准(详见校准机制)。
- **被读**:VideoSync / ProgressReporter 无锁读当前 pts(推算)。
- **freeze / unfreeze**:pause 时冻结(查询返回固定 pts),resume 时更新 pts 对应的系统时间。
- **jump_to(pos)**:seek 时时钟跳到目标 pts(连续标量,不能靠世代号丢弃识别——这是 seek.md 第⑤步专门调它的原因)。
- **reset(0)**:open/close 时归零冻结。

---

## 核心机制:两层时钟(校准层 + 推算层)

时钟分两层。校准层只在 miniaudio 回调里跑,算 ground truth;推算层给查询者,在两次校准间用 wall clock 插值。每次校准 = 用校准层结果刷新推算层基准。

### 校准层(回调里,精确,ground truth)

miniaudio 回调每次喂声卡 N 个样本后,用**绝对值**校准当前 pts:

```
当前 pts = first_pts + (provided − buffered) / sample_rate
                    ↑           ↑
              已提供给 miniaudio    声卡 buffer 残留
              的样本总数       (还没真播出去的)
```

- `first_pts`:首帧音频的 pts(**取自第一块喂声卡的 PCM 块的 pts,不是算出来的**)。整个会话不变,直到 reset/jump_to 重置。详见"首帧 pts 的确立"。
- `provided`:累计已提供给 miniaudio 的样本数。
- `buffered`:声卡 buffer 里还没播出去的样本数(声卡 latency)。
- `sample_rate`:采样率。

**为什么用绝对值而非累加**:`provided - buffered` 反映**声卡的真实播放位置**(已播 = 提供了但不再在 buffer 里)。每次回调都从这个绝对值重算,误差不累积、每次校准都重新对齐 ground truth。简单累加(每次 +N/采样率)会因浮点/除法误差漂移,且不知道 buffer 积压了多少没真播。

> 这是专业播放器(ffplay/mpv)的做法:用声卡 latency 做时钟校准。声卡以恒定采样率消耗样本,是系统最稳的时间基准。

### 首帧 pts 的确立(关键)

`first_pts` 是校准公式的**固定基准**,取自第一块喂给声卡的音频 PCM 块的 pts(AudioResampledStore 的每块带 pts)。判断"是不是第一块"靠一个内部标志 `first_pts: Option<Pts>`(None = 未确立):

```
calibrate(block_pts, samples_this_call, buffered_samples):
    if first_pts is None:              # 第一次:确立基准
        first_pts = block_pts          # 取自这块 PCM 的 pts
        provided = samples_this_call
    else:                              # 后续:只累加计数
        provided += samples_this_call
    ground_truth = first_pts + (provided − buffered_samples) / sample_rate
    base_time = now; base_pts = ground_truth
```

- `reset(pts)` / `jump_to(pos)` 会清除 first_pts(置 None),下次 calibrate 重新确立。
- 后续 calibrate 的 block_pts 可忽略(已用样本计数推进,不再靠 block_pts)。

### 推算层(查询时,插值)

校准只在回调时发生,查询者随时来读拿不到当前 buffer 残留,所以在两次校准间用 wall clock 插值:

```
查询当前 pts:
    if frozen:
        return frozen_pts                          // 暂停时返回固定值
    else:
        return base_pts + (now - base_time) × rate
```

- `base_time` / `base_pts` / `rate` / `frozen` / `frozen_pts` 组成复合状态。
- 每次校准(回调)用校准层结果刷新:`base_time = now, base_pts = 校准算出的 pts`。wall clock 漂移不累积(每次校准重新对齐)。

### 两层协作

```
miniaudio 回调(喂一块 PCM,带 block_pts + N 样本):
    if first_pts is None:
        first_pts = block_pts; provided = N        # 第一次:确立基准(取自块 pts)
    else:
        provided += N                              # 后续:累加计数
    校准 pts = first_pts + (provided − latency_samples) / sample_rate   ← 校准层 ground truth
    base_time = now; base_pts = 校准 pts                                 ← 刷新推算层基准

VideoSync / ProgressReporter 读:
    base_pts + (now − base_time) × rate                                  ← 推算层插值
```

---

## 线程可见性

无线程,被多线程读写:
- **写者**:AudioSink(miniaudio 实时线程,校准基准);ApiLoop(freeze/unfreeze/jump_to/reset)。
- **读者**:VideoSync、ProgressReporter(各自工作线程)。

**原子性**:`(base_time, base_pts, rate, frozen, frozen_pts)` 是复合状态,多线程读需一致快照。用 `std::atomic<std::shared_ptr>` 整体替换指针——读端无锁、写端罕见(校准/冻结/跳点)。与 api_layer.md 里 MediaInfo 的方案一致。

---

## 依赖

- **构造期注入**:无(纯资源,第 0 层,被所有人依赖)。
- **运行时被注入给**:AudioSink(写)、VideoSync / ProgressReporter(读)、ApiLoop(控制)。

---

## 状态机(内部,无对外可见状态)

AudioClock 自身无"模块状态机"(它是标量资源,不是工作模块)。但它有一个内部的**冻结位**影响查询行为:

```
frozen=true ──unfreeze()/校准──▶ frozen=false ──freeze()──▶ frozen=true
```

- open 后 reset(0):frozen=true,base_pts=0(冻结在 0,等 play)。
- play 启动出声:miniaudio 第一次回调校准 → frozen=false,base_time/base_pts 确立。
- pause:freeze() → 立刻把当前推算 pts 存为 frozen_pts,frozen=true。
- resume:unfreeze() → 用 frozen_pts + now 重置 base(base_time=now, base_pts=frozen_pts),frozen=false。
- seek:jump_to(pos) → base_pts=pos, base_time=now(无论冻结与否)。
- close:reset(0) → 归零,frozen=true。

---

## 对外接口(高层,不含内部实现)

| 方法 | 调用时机 | 职责 |
|------|---------|------|
| `reset(pts)` | open / close 命令 | base_pts=pts, base_time=now, frozen=true(冻结基准,等 play) |
| `calibrate(block_pts, samples_this_call, buffered_samples)` | AudioSink miniaudio 回调 | 首次确立 first_pts(取 block_pts);算 ground truth pts,刷新 base_time/base_pts;若 frozen 则 unfreeze |
| `current_pts()` | VideoSync / ProgressReporter 查询 | 无锁读:if frozen return frozen_pts else base_pts + (now-base_time)×rate |
| `freeze()` | pause 命令 | 算当前 pts 存为 frozen_pts,frozen=true |
| `unfreeze()` | play(pause 后恢复)命令 | base_time=now, base_pts=frozen_pts, frozen=false |
| `jump_to(pos)` | seek 命令 | base_pts=pos, base_time=now(连续标量跳点,不靠丢弃) |

> 接口签名细节(参数类型、std::atomic<std::shared_ptr> 怎么用)归实现阶段。

---

## seek 响应

AudioClock 的 seek 是 `jump_to(pos)`:把 base_pts 设为 pos、base_time 设为 now。这是 seek.md 编排的第⑤步(也是最后一步),在 demuxer/decoder/resampler 都 seek 完后调。

**为什么时钟要单独 jump_to 而不能靠世代号**:世代号机制防"旧数据混杂"(队列里旧包被丢弃),但时钟是**连续标量**——它没有"新旧数据"之分,不能靠"丢弃识别"。seek 到 100s,时钟必须显式跳到 100s,否则 VideoSync 还按旧 pts 选帧。这是 seek 三层数据正确性之外、时钟独有的第④件事(见 seek.md)。

---

## 关键设计决策

### 音频主时钟(声卡为基准)
声卡以恒定采样率消耗样本,已播放样本数 = 精确流逝的音频时间。这比系统时钟(wall clock 会漂移、受 NTP 调整影响)和视频帧 PTS(受解码抖动影响)都稳。音频主时钟是 A/V sync 的标准做法。

### 绝对值校准(防累积漂移)
用 `first_pts + (provided − buffered) / sample_rate` 而非"每次回调 +N/采样率"累加。前者每次校准都从绝对值重算,误差不累积;后者浮点/除法误差越积越大,且不知 buffer 积压。这是用声卡 latency 做校准的核心。

### 两层时钟(校准层 + 推算层)
校准层(回调里,拿得到 buffer 残留)算 ground truth;推算层(查询时,拿不到 buffer 残留)用 wall clock 插值。每次校准刷新推算层基准。wall clock 漂移不累积(每次校准重新对齐)。两层分工让"精确校准"和"随时可查"兼得。

### 暂停冻结(frozen 状态位)
暂停时 wall clock 还在走,若查询用 `base_pts + (now - base_time)` 会得到不断增大的错误 pts。故引入 frozen 位:暂停时存 frozen_pts、查询返回固定值;恢复时用 frozen_pts 重置 base。ProgressReporter 暂停报固定位置、VideoSync 暂停停贴帧,都靠这个。

### std::atomic<std::shared_ptr> 原子快照
复合状态(base_time/base_pts/rate/frozen/frozen_pts)多线程读需一致。std::atomic<std::shared_ptr> 整体替换指针,读端无锁、写端罕见。不拆成多个 AtomicXX(会撕裂)。

### 变速 rate 字段预留
rate 本阶段固定 1.0,但纳入映射公式(`× rate`)。set_speed 来时改 rate 即可,不用改时钟结构。真变速不变调需 SoundTouch(在 AudioResampler),时钟只反映 rate 变化。

---

## 坑与边界

### 声卡 latency 的获取
`buffered`(声卡 buffer 残留样本数)需通过 miniaudio latency API(`stream.latency()`,平台支持时)或自维护 ring 计数(送多少 vs 估计播了多少)获取。平台差异在实现阶段处理,不影响架构。

### 首帧 pts 的确立
`first_pts` 是校准公式的固定基准,**取自第一块喂声卡的 PCM 块的 pts**(AudioResampledStore 的每块带 pts),不是算出来的。判断"第一块"靠 `Option<Pts>` 标志(None=未确立):reset/jump_to 清除,第一次 calibrate 确立。若首帧 pts ≠ 0(如从中间 seek 起播),校准自动对齐到真实首帧 pts。

### seek 后 first_pts 必须重置(否则时钟被拉回旧位置)
seek 时 `jump_to(pos)` 设 base_pts=pos,但若不重置 first_pts,会出现 bug:
- seek 后音频链路重新定位+解码+重采样需要时间,这期间 miniaudio 回调还在跑,buffer 里可能还有**旧世代数据**。
- 若 AudioSink 播旧世代数据时还调 calibrate(用旧 first_pts + 旧 provided),算出的 ground truth 是**旧位置**,会**覆盖 jump_to 设的 base_pts=pos**——把时钟从 pos 拉回旧位置。

**解法(两层配合)**:
- `jump_to(pos)` 时**清除 first_pts**(重置标志),后续新数据第一次 calibrate 重新确立。
- **AudioSink 丢弃旧世代数据时不调 calibrate**(喂静音即可,与 AudioSink"丢弃旧 generation"现有职责一致)。只有**新世代数据**才 calibrate。

时序:
```
jump_to(pos):  base_pts=pos, first_pts=None(清除)
[旧世代数据被 AudioSink 丢弃,喂静音,不 calibrate]  ← base_pts 保持 pos
[新世代数据到达]
第一次新 calibrate:  first_pts=新块pts, 算 ground_truth ≈ pos, 修正 base  ← 真实数据对齐
```

> 新块 pts 可能 ≠ pos(FFmpeg seek 到最近关键帧 + PTS 过滤,pts 略 ≥ pos)。第一次 calibrate 用真实 pts 修正 jump_to 的估计值——jump_to 先给个估计,calibrate 用真实数据精校。

### miniaudio 回调里的开销
calibrate 在 miniaudio 实时线程执行,必须极轻量(算术 + std::atomic<std::shared_ptr> store),不拿重锁、不干重活、不 malloc。std::atomic<std::shared_ptr> store 是无锁原子操作,符合实时约束。

### jump_to 与冻结态
jump_to 无论 frozen 与否都更新 base_pts/base_time。若 seek 时正在暂停(frozen=true),jump_to 后查询仍返回 frozen_pts(因为 frozen 位还在)——这是对的:暂停态 seek 后保持暂停在新位置,unfreeze 时才从新 pts 起播。

---

## 边界（本文档不涉及）

- ❌ miniaudio latency API 的具体使用 / 自维护 ring 计数的实现 → 实现阶段
- ❌ std::atomic<std::shared_ptr> 的具体用法 / 状态结构体的字段布局 → 实现阶段
- ❌ 变速不变调（SoundTouch）的接入 → AudioResampler 文档 / 未来阶段
- ❌ VideoSync 如何用 current_pts 选帧 → video_sync.md（待设计）
