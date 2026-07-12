# VideoSync 模块设计

> 末端同步模块。音视频同步的最后一环——读 AudioClock 的当前 pts,从 FinalFrameStore 选该显示的帧,交付 Flutter。
> 对外接口由 ApiLayer 调用(start/pause/stop)。本文件描述其内部设计,核心是 A/V sync 选帧算法。

## Context

播放器的视频画面要和音频对齐——音频是主时钟(声卡恒定采样率,最稳),视频要"追"音频。VideoSync 就是这个"追"的执行者:它读 AudioClock 的 `current_pts()`,从 FinalFrameStore 里选 PTS 最接近 current 的帧,决定何时交付、何时丢、何时等。

这是 A/V sync 的核心,也是音视频面试的高频考点。VideoSync 的设计本质是"视频帧 PTS 与音频时钟 current_pts 的差值分类决策"。

---

## 定位

```
FinalFrameStore(gen, CPU RGBA) ──→ [VideoSync] ──→ Flutter texture
                                         ↑
                                  AudioClock.current_pts()
                                         ↑
                                  ApiLoop 控制: start/pause/stop
```

- **工作模块层,1 个 loop 线程**(方案 Y 常驻)。
- **末端消费者**:读 FinalFrameStore + AudioClock,产出交付 Flutter。不喂任何下游 Store。
- **copy-back 路下**:FinalFrameStore 是 CPU RGBA buffer,VideoSync 选完帧内部上传成 Flutter texture(这一跳很薄,归 VideoSync 内部,不单独模块)。
- **seek 零改动**:无 seek() 方法,靠世代号丢弃 FinalFrameStore 旧帧 + 自然保持上一帧自洽(见设计决策)。

---

## 职责

- **选帧**:从 FinalFrameStore 取 PTS ≤ current_pts 且最近的帧(标准做法)。
- **同步决策**:按"帧 PTS 与 current_pts 的差值"分四区间决策(丢帧/赶/交付/精准等)+ 无数据死等通知。
- **交付 Flutter**:选定的帧上传成 Flutter texture 并触发重绘(copy-back:CPU buffer→texture 上传)。
- **异常监控**:diff 异常大时(视频严重超前,说明音频卡顿/时钟出问题)上报异常通知。

**不做**:
- ❌ 不解码/转换/合成(上游模块的事)。
- ❌ 不驱动音频(AudioClock 由 AudioSink 驱动,VideoSync 只读)。
- ❌ 不决定播什么(只选"现在该显示哪帧",不改变帧内容)。
- ❌ 不做 configure(不需媒体格式配置,读的接口固定)。

---

## 核心算法:四区间选帧决策 + 无数据死等

视频帧 PTS 与 AudioClock `current_pts()` 的差值 `diff = frame.pts - current_pts`。有帧时分四种情况,无帧时单独处理:

```
   帧 PTS 远 < current    帧 PTS 略 < current    帧 PTS ≥ current
   ◄──────────────────►◄──────────────────►◄────────────────►
        落后太多              落后不多          超前(含同步)
```

| 情况 | 条件 | 策略 | 理由 |
|------|------|------|------|
| **落后太多** | diff < -T落后 | **丢帧**:pop 掉这帧,continue 取下一帧 | 视频严重滞后,逐帧丢直到追上或到能用的帧 |
| **落后不多** | -T落后 ≤ diff < 0 | **立即交付该帧**,不丢不 sleep | 略旧的帧还能用,立即交付让显示跟上,视频"快进一点"自然追上 |
| **超前(含同步)** | diff ≥ 0 | **sleep diff 精准等到该帧 PTS 时刻**再交付;diff 异常大时上报异常 | 视频跑到音频前面了,等到该显示它的时候。无论超前多少都能算出精准等待时长 |
| **无数据** | Store 空 | **wait(None) 死等通知** | 无帧可算 diff,只能等 FinalReady/控制信号唤醒 |

### 为什么"超前"统一 sleep diff 精准等(不区分超前多少)

无论超前 30ms 还是 300ms,都能算出 `diff = frame.pts - current` 这个精准等待时长。sleep diff 后醒来,音频时钟已推进到 ≈ frame.pts,diff 变 ≈ 0,该帧从"超前"变"同步",交付即可。所以"超前不多"和"超前太多"是**同一个处理**(sleep diff 交付),只是 sleep 时长不同,不需要分两个区间。

> 之前的"超前太多保持上一帧"是基于"怕 sleep 太久出问题"的误判——sleep diff 是精确计算,睡多久取决于 diff 多大,没有"睡太久"的问题。保持上一帧反而错了:它让画面冻住,而实际上该帧睡一会儿就能正常交付。

### 异常监控:diff 过大上报

异常监控从"保持上一帧时监控"改成"diff 过大时上报":如果 diff 大得离谱(如 > 5s,远超正常超前量),说明音频严重卡顿/时钟出问题,通过 Notifier 上报 `VideoSyncStalled` 通知,ApiLayer 决定怎么处理。这是**诊断信号**,不影响选帧动作(还是 sleep diff 交付)。比"固定时长停滞才报"更准——diff 过大直接反映音频出了问题。

### 无数据死等通知(不掩盖 bug)

无帧时 wait(None) 无限等,只靠 FinalReady(来新帧)和控制信号(pause/stop)唤醒。**不加兜底超时自醒**——如果唤醒丢了那是 Notifier/控制信号的 bug,该暴露不是该掩盖。加兜底超时反而隐藏 lost wakeup bug(唤醒丢了 100ms 后自醒"正常"了,你以为没问题其实机制坏了)。死等能让 bug 显形(画面冻住不动,立刻知道唤醒链路坏了)。和 Demuxer 线程模型一致(纯等通知,无兜底超时)。

### 阈值(实现时调参,初值参考)

- **T落后(丢帧阈值)**:1-2 个视频帧时长(30fps → 33-66ms)。超过才丢,否则"快速追赶"。
- **T异常(diff 过大阈值)**:如 5s。diff 超过此值上报异常(诊断音频卡顿/时钟问题)。

> 阈值不写死,架构定"有这几个阈值 + 各自触发什么策略",实现时按实测调参。

---

## 线程模型:事件驱动,自己知道何时醒

VideoSync **不用定时器轮询**,它该在"有新帧可用 且 该显示它的时间到了"时醒。唤醒源:

VideoSync **不用定时器轮询**,它该在"有新帧可用 且 该显示它的时间到了"时醒。唤醒源:

1. **FinalFrameStore 来新帧**(Notifier 通知 FinalReady)→ notify cv,醒来看新帧的 PTS 和 current 关系。
2. **sleep 超时**(超前时算了 diff,sleep diff 时长)→ 超时也是 notify cv,醒来交付该帧。
3. **控制信号**(pause/stop)→ notify cv,醒来检查 state。

无数据时没有 sleep(无可算的等待时长),只靠源 1 和 3 唤醒(死等)。

这样醒的时机**精准对齐"该换帧的时刻"**,不空转、不抖动。和 Demuxer 线程模型一致(cv.wait + Notifier 回调唤醒 + 循环顶重新检查)。

### 选帧循环

```
VideoSync loop:
    loop:
        wait on cv (被 FinalReady / sleep超时 / 控制信号 唤醒)
        回到循环顶检查 state:
            if state == Paused: continue wait      # 暂停,保持当前帧
            if state == Stopping: 退出 loop
        
        final = FinalFrameStore.try_latest()       # 取最新帧(查 generation,旧世代丢弃)
        if final is None:
            wait(None)                             # 无数据死等通知(不加兜底超时,掩盖bug)
            continue
        
        diff = final.pts - audio_clock.current_pts()
        if diff < -T落后 (落后太多):
            FinalFrameStore.pop()                  # 丢帧
            continue                               # 取下一帧看
        elif diff < 0 (落后不多):
            交付 final (上传 texture), continue    # 快速追赶(不sleep)
        else: # diff >= 0 (超前或同步)
            if diff > T异常: 上报 VideoSyncStalled  # 诊断,不影响动作
            wait(Some(diff))                       # 精准等到该帧 PTS
            continue                               # 醒来回循环顶重新评估
```


---

## 状态机

```
Constructed ─start()─▶ Running ⇄ Paused
   ▲                       │
   │                       └─stop()/close()─▶ Stopping ─▶ Stopped
   └──────────────────────────────────────────┘
   (Stopped 后不再回 Constructed;模块对象在 close 时复用,真正销毁归 shutdown)
```

| 状态 | 含义 | 线程 | 选帧循环 |
|------|------|------|---------|
| **Constructed** | init 装配完,空壳 | 未起 | — |
| **Running** | play 启动后,选帧循环在跑 | 在跑 | 活跃(选帧/交付/sleep) |
| **Paused** | pause 后,冻结 | wait 在 cv | 停止选帧(保持上一帧) |
| **Stopping** | 收到 stop/close,线程准备退出 | 即将退出 | 停止 |
| **Stopped** | 线程已退出 | 已退出 | — |

### 各状态行为

**Running**:选帧循环活跃(上面的 loop)。被 FinalReady/sleep 超时/控制信号唤醒,按四区间 + 无数据决策。

**Paused**:pause() 设 state=Paused + notify cv。线程醒来发现 Paused,**不选新帧,保持当前纹理**(Flutter 画面停住)。wait 在 cv 直到 play() 的 start() 唤醒。
- 关键:Paused 时 AudioClock 也冻结(current_pts 不变),即使醒了选帧 diff 也≈0 本就不会换——但显式 Paused 更清晰,避免无意义循环。

**Running ⇄ Paused 的纹理保留**:pause 时纹理停在当前帧,resume 从当前帧继续。VideoSync 不主动清纹理,pause/resume 期间 Flutter 持续显示最后一帧(play.md"pause 停在当前帧,纹理保留"的落地)。

**Stopping → Stopped**:stop() 设 Stopping + notify cv。线程醒来发现 Stopping,退出 loop → Stopped。close 时 stop() 等线程 join,对象留着复用。

### 线程生命周期(方案 Y,常驻)

- **首次 start 时 spawn**(不播放不占线程)。
- 常驻到 **close/shutdown**(Stopping)才退出。
- pause / 无数据死等 / 超前 sleep → 线程 **wait 在 cv**(不退出)。
- close 后线程没了,下次 start 检查并重新 spawn。

---

## 对外接口

| 方法 | 调用者 | 职责 |
|------|--------|------|
| `start()` | play 命令 | 首次 spawn 线程;state=Running;notify cv 唤醒进入选帧循环 |
| `pause()` | pause 命令 | 冻结选帧(保持当前帧);state=Paused;线程 wait |
| `stop()` | close 命令 | state=Stopping + 唤醒,等线程退出(join) |

**无 seek()**——靠世代号丢弃 FinalFrameStore 旧帧 + 自然保持上一帧自洽(见设计决策)。
**无 configure**——不需媒体格式配置,读的 AudioClock/FinalFrameStore 接口固定。
**无对外选帧方法**——选帧是内部 loop 的事,外部只控制生命周期。

> 这是所有工作模块里接口最少的之一(只 start/pause/stop),因为 VideoSync 是末端消费者 + seek 零改动。

---

## 依赖

### 构造期注入(DI,Arc 持有)

| 依赖 | 用途 |
|------|------|
| `FinalFrameStore` | 取最新帧(选帧输入) |
| `AudioClock` | 读 current_pts(同步基准) |
| `Generation` | 丢弃旧世代帧(取 FinalFrameStore 帧时查 generation) |
| `Notifier` | 注册 FinalReady 通知(帧就绪被唤醒)+ 发送 VideoSyncStalled 通知 |

### 交付 Flutter(copy-back 路下)

VideoSync 选完帧后,内部把 CPU RGBA buffer 上传成 Flutter texture 并触发重绘。这一跳很薄(单次上传 + texture ID 管理),归 VideoSync 内部,不单独模块。**Flutter texture 对接的具体方式(ExternalTexture 注册/texture ID/上传时序)是实现时平台细节**,本文档不展开。

---

## 关键设计决策

### 选 PTS ≤ current 且最近的帧(标准 A/V sync)
音频是主时钟,视频追音频。取满足"PTS ≤ current"的最新帧,即"该显示的帧是已到播放时间且最近的"。这是 ffplay/mpv 等的标准做法。

### 四区间决策(丢/赶/交付/精准等)+ 无数据死等
diff 不是简单的"同步/不同步"二分,而是四区间:落后太多丢、落后不多赶、超前(含同步)sleep diff 精准等、无数据死等通知。区分"落后太多"和"落后不多"是关键——前者要丢帧止损,后者用略旧帧快速追赶(不丢不 sleep,自然追上)。

### 超前统一 sleep diff 精准等(不区分超前多少)
无论超前多少都能算出 `diff = frame.pts - current` 精准等待时长。sleep diff 后醒来音频时钟已推进,diff 变 ≈ 0,该帧从"超前"变"同步"交付。所以"超前不多/超前太多"是同一个处理(sleep diff 交付),只是 sleep 时长不同。之前的"超前太多保持上一帧"是基于"怕 sleep 太久"的误判——sleep diff 是精确计算,没有"睡太久"问题,保持上一帧反而让画面无谓冻住。

### 无数据死等通知,不加兜底超时
无帧时 wait(None) 无限等,只靠 FinalReady + 控制信号唤醒。不加兜底超时自醒——如果唤醒丢了是 Notifier/控制信号的 bug,该暴露不是该掩盖。兜底超时隐藏 lost wakeup bug(唤醒丢了自醒"正常"了,你以为没问题其实机制坏了)。死等让 bug 显形(画面冻住立刻知道唤醒链路坏了)。和 Demuxer 线程模型一致(纯等通知,无兜底超时)。

### 事件驱动,不用定时器轮询
VideoSync 不用"每 16ms 醒一次查"的定时器(大多数时候帧没变白醒、醒的时机和换帧时刻不对齐有抖动、和视频帧率不匹配)。而是 cv.wait 被精准唤醒:FinalFrameStore 来帧(FinalReady)+ sleep 超时(超前时算的 diff)+ 控制信号。醒的时机对齐"该换帧的时刻",不空转不抖动。和 Demuxer 线程模型一致。

### 异常监控:diff 过大上报
异常监控是"diff 过大上报 VideoSyncStalled"(诊断音频卡顿/时钟问题),不是"保持上一帧持续多久才报"。diff 过大直接反映音频出问题,比"固定时长停滞"更准。这是诊断信号,不影响选帧动作(还是 sleep diff 交付)。

### seek 不清除上一帧缓存(体验 + 正确性)
seek 后 VideoSync 保留 seek 前的最后一帧。新数据没到时,选不到新帧 → 无数据死等(保持上一帧)。新世代第一帧到达后才切换。
- **体验好**:seek 时画面停在最后一帧,等新数据无缝切换,不黑屏。
- **正确**:保持旧帧不是"播错位置"——AudioClock 已 jump_to(pos),VideoSync 醒来算 diff 发现"Store 无新帧"(走无数据死等)或"新帧 PTS 远 > current"(走超前 sleep diff 等待),画面不动。等新帧到(PTS≈pos)切换。世代号管 Store 里的旧帧(丢弃),VideoSync 内部的上一帧不受世代号管(已显示过的),自然保持。
- **唯一边界**:seek 跨度大(如 10s→100s)时,保持的是 10s 画面直到 100s 新帧到(<200ms),用户感知"短暂停顿后跳到新位置",可接受,比黑屏好。

### seek 零改动(无 seek() 方法)
基于上一条,VideoSync 靠世代号 + 自然保持自洽 seek,不需要显式 seek() 方法。这是 architecture.md"seek 逻辑零改动"原则向工作模块的延伸——只要工作模块内部状态能靠"保持/丢弃"自洽,就不需要显式 seek。对比 decoder 需要 seek()(要 flush 内部参考帧),VideoSync 更简单。接口只有 start/pause/stop。

### 无 configure
VideoSync 读 AudioClock + FinalFrameStore,接口固定,不需按媒体格式配置。和 decoder(configure 建解码器)/sink(setup 建 cpal 流)区别清晰——末端消费者不需要按媒体特性初始化。

---

## 坑与边界

### sleep 的精度
"超前 sleep diff 精准等"依赖 sleep 精度。std sleep 精度约 1-15ms(平台/调度),可能不准。实现时可考虑:① sleep 略短一点 + 醒后 busy-wait 精准对齐;② 或接受小抖动(A/V sync 容忍几 ms)。归实现阶段。

### FinalFrameStore 的"最新帧"语义
VideoSync 取 FinalFrameStore.try_latest()——是取"最新的一帧"还是"按 PTS 顺序的最旧可用帧"?倾向"最新满足 PTS ≤ current 的帧"。FinalFrameStore 内部实现(有界队列 vs 单帧快照)影响这个语义,见 architecture.md 待确认项"各 rendered Store 内部实现"。

### Flutter texture 上传的开销与时机
copy-back 路下每帧要 CPU buffer→texture 上传。上传在 VideoSync 线程做,要快(避免拖慢选帧循环)。Flutter 的 texture 注册/更新时序(和 Flutter vsync 对齐)是实现时平台细节。

### diff 异常大的误报
diff > T异常(如 5s)上报 VideoSyncStalled,但 seek 刚完成时新帧 PTS 可能远 > current(音频还没追到新位置),会短暂触发误报。实现时可加"seek 后宽限期"(seek 完成后 N 秒内不上报),或"diff 持续异常才报"(连续 M 次 diff 异常才上报)。归实现阶段调参。

### 丢帧的统计
落后太多丢帧时,可统计丢帧率(每秒丢多少)用于诊断。高频丢帧说明解码跟不上/系统卡,可上报。这是可选的运维功能,非核心。

---

## 边界（本文档不涉及）

- ❌ Flutter ExternalTexture 的注册/上传具体实现 → 实现阶段(平台细节)
- ❌ FinalFrameStore 的内部实现(有界队列 vs 单帧快照) → store 设计文档(待定)
- ❌ 阈值的具体值(T落后/T异常) → 实现阶段调参
- ❌ sleep 精度优化(busy-wait 等) → 实现阶段
- ❌ 丢帧率统计上报 → 可选运维功能,未来阶段
1. **FinalFrameStore 来新帧**(Notifier 通知 FinalReady)→ notify cv,醒来看新帧的 PTS 和 current 关系。
2. **sleep 超时**(超前时算了 diff,sleep diff 时长)→ 超时也是 notify cv,醒来交付该帧。
3. **控制信号**(pause/stop)→ notify cv,醒来检查 state。

无数据时没有 sleep(无可算的等待时长),只靠源 1 和 3 唤醒(死等)。

这样醒的时机**精准对齐"该换帧的时刻"**,不空转、不抖动。和 Demuxer 线程模型一致(cv.wait + Notifier 回调唤醒 + 循环顶重新检查)。

### 选帧循环

```
VideoSync loop:
    loop:
        wait on cv (被 FinalReady / sleep超时 / 控制信号 唤醒)
        回到循环顶检查 state:
            if state == Paused: continue wait      # 暂停,保持当前帧
            if state == Stopping: 退出 loop
        
        final = FinalFrameStore.try_latest()       # 取最新帧(查 generation,旧世代丢弃)
        if final is None:
            wait(None)                             # 无数据死等通知(不加兜底超时,掩盖bug)
            continue
        
        diff = final.pts - audio_clock.current_pts()
        if diff < -T落后 (落后太多):
            FinalFrameStore.pop()                  # 丢帧
            continue                               # 取下一帧看
        elif diff < 0 (落后不多):
            交付 final (上传 texture), continue    # 快速追赶(不sleep)
        else: # diff >= 0 (超前或同步)
            if diff > T异常: 上报 VideoSyncStalled  # 诊断,不影响动作
            wait(Some(diff))                       # 精准等到该帧 PTS
            continue                               # 醒来回循环顶重新评估
```

