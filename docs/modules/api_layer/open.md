# open 命令的编排设计

> 属于 ApiLayer 模块。描述 `open` 命令在 ApiLoop 中如何编排执行。
> 各模块内部 open 响应细节见各自模块文档（待设计）。

## Context

open 的职责是**打开媒体资源、建立播放前提**——让播放器从 Idle 进入 Ready（就绪、未播放）。open **不管水位、不启动播放管道**，数据流动交给 play。

**核心原则**：open 是"准备好能播的一切前提（除数据）"，play 是"让数据流动起来"。两者职责严格分离。

open 是昂贵操作（文件 I/O + 格式探测），但只执行一次（换媒体时）。命令走命令队列、完成才 resolve（返回 MediaInfo）。

---

## open 命令的编排（ApiLoop 内）

```
void handle_open(src):
    // ① 前置: 已有媒体则先 close 回 Idle
    if current_media != None:
        await close_internal()

    // ② 打开资源 + 探测 (open 唯一的重活, 必须等)
    media_info = demuxer.open(src)      // 探测 + 暴露流配置(video_config/audio_config, 纯数据)

    // ③ 配置音频链路会话上下文 (worker 线程由模块生命周期自持；此处不读数据)
    //    ApiLayer 协调 demux 和 decoder: demux 出配置, decoder 用配置自建解码器
    video_decoder.configure(demuxer.video_config)
        // decoder 用 config 自己建解码器(含硬解上下文); 自包含, 不感知"流"概念
    decoded = audio_decoder.configure(demuxer.audio_config)
        // 产出 decoder 实际输出的 PCM format
    output = audio_output.configure(options)
        // 探测/选择系统输出支持的 playback PCM format
    audio_resampler.configure(input=decoded.format, output=output.playback_format)
        // 按 input(解码后 PCM) → output(设备播放 PCM) 建 SwrContext; 自包含, 不感知"流"概念
    audio_clock.reset(0)                     // 时钟基准归 0, 冻结

    // ④ 会话状态
    player_state = Ready                // 就绪、未播放
    target_start_pts = 0                // 默认从头 (Ready 态 seek 可改它)
    current_media = Some(src)
    duration = media_info.duration_us

    handle.resolve(Ok(media_info))      // 返回 MediaInfo 给 Dart
```

---

## 各步的职责（高层，不含内部实现）

| 步骤 | 模块方法 | 高层职责 | 内部细节归属 |
|------|---------|---------|------------|
| ① | `close_internal()` | 若已有媒体，清理旧媒体回 Idle | close.md（待设计）|
| ② | `demuxer.open(src)` | 探测流信息（编码/分辨率/时长）→ MediaInfo + 暴露流配置（video_config/audio_config 纯数据） | demuxer.md（待设计）|
| ③ | `video_decoder.configure(config)` | 用 config **自己建**视频解码器（含硬解上下文）；自包含，不感知流概念 | video_decoder.md |
| ③ | `audio_decoder.configure(config)` | 用 codec config 自己建音频解码器，并返回实际 decoded PCM format | audio_decoder.md |
| ③ | `audio_output.configure(options)` | 探测/选择输出设备支持的播放格式，建输出后端上下文，返回 `playback_format` | audio_output.md |
| ③ | `audio_resampler.configure(input, output)` | 用 decoded PCM → playback PCM 建 SwrContext；自包含，不感知流概念 | audio_resampler.md |
| ③ | `audio_clock.reset(0)` | 时钟基准 PTS=0，冻结 | audio_clock.md |
| ④ | 会话状态 | `player_state=Ready`、`target_start_pts=0`、记 current_media/duration | ApiLayer 内部 |

---

## 关键设计决策

### open 不管水位
open 不预填充队列、不启动解封装/解码线程。只做"打开 + 探测 + 配置解码器 + Ready"。数据流动完全交给 play。这样 open 快（只等探测拿 MediaInfo），Dart 能立刻拿到媒体信息显示 UI。

### open 完成 = 探测完成
open 的 resolve 只等探测阶段（`demuxer.open` 拿到 MediaInfo），不等预填充。MediaInfo 在探测阶段就有，无需等队列填满。

### configure 配置会话上下文，但不读数据
③ 步的 `configure` 只建立当前媒体会话所需的后端上下文（解码器、重采样器、输出后端）。工作模块的 worker 线程由 IoC 装配后的模块生命周期自持；`open` 不启动数据读取，也不预填队列。数据流动仍由后续 `play` 语义接管。

### decoder 用 configure 自建解码器（自包含，不感知流）
- ApiLayer 协调：demux 探测后暴露**流配置（video_config/audio_config，纯数据）**；decoder 收到 config 后**自己建解码器**（含硬解上下文）。
- decoder **不感知"流"概念**（不知道流名/流序/媒体结构），只面对"一份配置 + packet_queue + frame_store"。
- decoder **运行时只依赖队列**（取包解包）；建解码器是一次性初始化，走 configure 接口。

### 建解码器的配置不挂队列（两类信息分开）
流信息分两类，去向不同：
- **流级静态配置**（codecpar/extradata，如 H.264 的 SPS/PPS）：整条流一份、一次性。走 `configure` 接口传给 decoder，**不进数据流、不每包冗余**。理由：建解码器只需一次，且 open 时队列还空着；放队列是位置错、时机错、且让队列越界懂"decoder 要什么"。
- **包级动态标记**（世代号、流 id）：随包流动，**该挂队列**（贴在每个包上）。这是数据流的有机部分。

demux 只提供配置数据（纯数据），**不替 decoder 建解码器实例**——避免 demux 越界，保持 decoder 自包含。

### open 前先 close
open 不是"在已有媒体上打开新文件"，而是"关掉旧的、开新的"。若 `current_media != None`，先 `close_internal()` 回 Idle，再 open。

### 状态 = Ready（不是 Playing）
open 后不自动播放。`player_state = Ready`（就绪未播），时钟冻结在 0。`play()` 才进入 Playing。

### AudioOutput 探测输出格式，但不出声
AudioOutput 在 open 阶段根据系统/策略产出 `playback_format`，这份格式是 AudioResampler output format 的权威来源。输出后端可以建立必要上下文，但不会因为 open 完成就播放声音；真正出声仍由后续播放控制驱动。

---

## Ready 态的 seek（调起点）

open 后处于 Ready 态。**Ready 态 seek 合法**，语义是"调整播放起始位置"，但**不启动 demuxer、不做真正 FFmpeg 定位**：

```
Ready 态 seek(pos):
    target_start_pts = pos        // 只记目标, 不动 demuxer (管道还是冷的)
    player_state 保持 Ready
    handle.resolve(Ok(()))
```

当前 `demuxer.seek()` 只记录目标，尚未执行真正的 FFmpeg 定位（`av_seek_frame`）。
待 `DemuxerBackend` 增加 seek 契约后，再由 ApiLayer 编排定位、generation 推进和下游 flush。

---

## 与 play 的职责切分

| 操作 | 做什么 | 不做什么 |
|------|--------|---------|
| **open** | 打开文件、探测拿 MediaInfo、建各模块上下文、状态=Ready、target_start_pts=0 | 不填水位、不启动管道、不动 demuxer 读 |
| **play** | 启动 demuxer/decoder、填到水位、解冻时钟+出声 | 当前不执行 target_start_pts 的 backend 定位；也不管打开/探测（open 已做） |

详见 `play.md`。

---

## 状态机（open 相关）

```
Idle ──open()──▶ Ready
                  │
                  ├── play() ──▶ Playing
                  ├── seek(pos) [Ready态, 调起点, 只记 target_start_pts]
                  └── close() ──▶ Idle
```

---

## 边界（本文档不涉及）

- ❌ 各模块 configure/setup 内部实现 → 各模块文档
- ❌ play 的冷启动填水位 → play.md
- ❌ close 的资源清理编排 → close.md
- ❌ Ready 态 seek 的详细语义（target_start_pts 如何被未来的 seek 契约消费）→ play.md
