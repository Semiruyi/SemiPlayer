# AudioResampler 模块设计

> 音频重采样模块。位于 AudioDecoder 与 AudioSink 之间，把解码原始 PCM 转成 miniaudio 输出目标格式。
> 对外接口由 ApiLayer 调用（见 `docs/modules/api_layer/` 各命令编排）。本文件描述其内部设计。

## Context

当前音频链路被悄悄假设了一个前提：open.md 里 `audio_sink.setup(demuxer.audio_config)` 用**解码侧格式**直接建 miniaudio 流。但 FFmpeg 解出的 PCM 格式（采样率/声道/采样格式/planar↔packed）由**流的编码**决定，miniaudio 能输出的格式由**平台/设备**决定，**两者未必一致**（如流是 44100/s16/5.1，miniaudio 只支持 48000/f32/stereo）。

AudioResampler 填这个 gap：把解码原始 PCM 转成 miniaudio 能吃的格式。顺带解放"AudioSink 必须支持解码格式"这个不成立的前提——miniaudio 按自己支持的格式建流，重采样负责对齐。

---

## 定位

```
AudioPacketQueue(gen) →[AudioDecoder]→ AudioFrameStore(gen, 解码原始PCM)
                                        →[AudioResampler]→ AudioResampledStore(gen, miniaudio目标格式PCM) →[AudioSink]→ 声卡
```

- 位于 AudioDecoder 与 AudioSink **之间**。
- DAG 层级：**第 1 层**（与 AudioDecoder 同层）。它依赖 AudioFrameStore（第 0 层资源）、产出 AudioResampledStore（第 0 层资源），**不依赖 AudioDecoder 这个模块本身**（只接它的产出 Store），故与 decoder 同层、无环。
- **不依赖 AudioSink**：目标格式是 open 时定的静态配置（见 configure），不是运行时服务。避免"AudoSink 取 ResampledStore + Resampler 取 AudioSink 格式"成环，DAG 干净。

---

## 职责

- **格式转换**：解码原始 PCM（采样率 / 声道数 / 采样格式 / planar↔packed）→ miniaudio 输出目标格式。底层用 FFmpeg `swr_convert`。
- **流式 + 有状态**：重采样是流式计算，内部有滤波器历史样本，需维护重采样器状态（`SwrContext`）。
- **seek 响应**：flush 内部残留样本（否则跨 seek 串音）+ 按 generation 丢弃旧世代数据。
- **预留**：变速不变调（`set_speed`）——`swr_convert` 只改采样率会变调，真变速需 SoundTouch 之类，本阶段不做，但模块位置留好。

**不做**：
- ❌ 不解码（AudioDecoder 的事）。
- ❌ 不输出到声卡（AudioSink 的事）。
- ❌ 不决定目标格式（由 miniaudio/设备能力决定，open 时协商一次传给它，见 configure）。

---

## 线程

**1 个 loop 线程**，和其他工作模块一致。

**为什么不在 miniaudio 实时线程**：重采样是**有状态的计算**（滤波器历史），`swr_convert` 内部会分配缓冲。miniaudio 实时线程要求零阻塞、零 malloc，放进去是反模式。符合现有架构哲学：**每个工作模块一线程、靠 Store 解耦、miniaudio 实时线程只做 try 取 + 喂声卡（零计算）**。

**节奏**：AudioFrameStore 非空 → Notifier 唤醒 → 取 raw PCM → swr_convert → 推 AudioResampledStore（满则背压 wait）。

---

## 依赖

### 构造期注入（DI，shared_ptr 持有）

| 依赖 | 用途 |
|------|------|
| `AudioFrameStore` | 取解码原始 PCM（输入） |
| `AudioResampledStore` | 推重采样后 PCM（输出） |
| `Generation` | 丢弃旧世代数据 |
| `Notifier` | 注册 AudioFrameStore 非空 / AudioResampledStore 非满通知，被唤醒 |

### open 时 configure 注入（纯数据，非运行时依赖）

- 目标格式 config（采样率 / 声道 / 采样格式）——来自 AudioSink 探测 miniaudio 设备能力后产出的 `audio_output_config`。
- 与 open.md 里"decoder 用 configure(config) 自建"是**同一模式**：ApiLayer 协调，AudioSink 探测产出 output_config，`audio_resampler.configure(input=解码格式, output=miniaudio格式)`。

> 关键：**目标格式是 open 时定的静态配置，不是运行时服务**。所以 AudioResampler 不运行时依赖 AudioSink，避免"AudoSink 取 ResampledStore + Resampler 取 AudioSink 格式"成环。DAG 干净。

---

## 状态机

```
Constructed ─configure()─▶ Idle ─start()─▶ Running ⇄ Paused
   ▲                                    │
   │                                    └─stop()/close()─▶ Stopping ─▶ Stopped ─unconfigure()─▶ Constructed
```

| 状态 | 含义 | 线程 |
|------|------|------|
| **Constructed** | init 装配完，空壳，无重采样器 | 未起 |
| **Idle** | configure 后，重采样器已建（知道目标格式），未在跑 | 未起 |
| **Running** | start 后，线程重采样循环在跑 | 在跑 |
| **Paused** | pause 后，线程 wait（不退出，背压/暂停自然停）| wait 在 cv |
| **Stopping** | 收到 stop/close，线程准备退出 | 即将退出 |
| **Stopped** | 线程已退出 | 已退出 |

> 线程生命周期同 Demuxer 方案 Y：**首次 start 时 spawn，常驻到 close/shutdown**。pause / 输入空 / 输出满 → 线程 wait 在 cv（不退出）。close 后线程没了，下次 start 检查并重新 spawn。

---

## 对外接口（高层，不含内部实现）

| 方法 | 调用时机 | 职责 |
|------|---------|------|
| `configure(input_format, output_format)` | open 命令 | 建 SwrContext（按 input/output 格式参数）；state = Idle。**不启动线程** |
| `start()` | play 命令 | 首次 spawn 线程；state = Running；notify cv 唤醒 |
| `seek(pos)` | seek 命令 | flush 内部重采样器残留（`swr_close`/重建）+ 记 target_pts；与世代号配合丢旧世代数据 |
| `stop()` | close 命令 | state = Stopping + 唤醒，等线程退出（join）|
| `unconfigure()` | close 命令 | 释放 SwrContext（configure 的逆）；state = Constructed |

> 接口签名细节（flush 怎么调、target_pts 怎么存）归实现阶段。

---

## seek 响应（世代号 + flush）

AudioResampler 的 seek 涉及两层正确性，与 decoder seek 一致：

- **第①层 内部残留样本**：重采样器滤波器里有历史样本，跨 seek 会串音。→ `seek()` 内部 flush（`swr_init` 重置或重建 SwrContext）。
- **第②层 跨 seek 旧数据**：输入队列里的旧世代 PCM。→ 按 generation 丢弃（取 AudioFrameStore 数据时查 generation，不等则丢）。

> 第③层（PTS 过滤）对纯音频重采样意义不大（音频不像视频有"关键帧定位到目标前"的副作用），但若上游 AudioDecoder 已按 target_pts 过滤，到这里的帧 PTS 都 ≥ target，无需 Resampler 再过滤。

---

## 对现有架构的连带影响

1. **模块清单**：工作模块层 +1 `AudioResampler`（1 个 loop 线程）；资源管理者层 +1 `AudioResampledStore`（无锁 SPSC `鏃犻攣 SPSC 鐜舰闃熷垪`，每块带 generation，与现有 Store 同模式）。
2. **数据流图**：音频链路从 3 段变 4 段（多 Resampler 一跳）。
3. **open 编排**（open.md）：`audio_sink.setup` 改为"探测 miniaudio 能力 → 产出 output_config"；新增 `audio_resampler.configure(input, output)`。
4. **seek 编排**（seek.md）：handle_seek 从 4 步变 5 步，新增 `audio_resampler.seek(pos)`。
5. **close 编排**（close.md）：新增 `audio_resampler.stop()` + `unconfigure()`，位置在 `audio_decoder.stop()` 之后、`audio_sink.teardown()` 之前。

---

## 关键设计决策

### 独立成模块而非塞进 AudioSink / AudioDecoder
- **不塞进 AudioSink**：AudioSink 复用 miniaudio 实时线程，重采样是有状态计算 + 会 malloc，放进去违反实时约束。
- **不塞进 AudioDecoder**：重采样的目标格式由 miniaudio 能力决定（输出侧），而 decoder 只懂流的编码（输入侧）。把输出侧知识塞进 decoder，破坏"decoder 只面对一份配置 + 队列"的自包含原则。
- **独立模块 + 独立 Store**：每个工作模块产出一个 Store，模式统一、可单测（构造 Resampler 不需先构造 miniaudio/Sink）。

### 目标格式 open 时定，非运行时协商
目标格式是静态配置（miniaudio 设备能力在 open 时探测一次）。Resampler configure 一次后运行时只做转换，不查格式。这让 Resampler 不运行时依赖 AudioSink，DAG 干净。

### 预留变速不变调
`set_speed` 若启用，需在重采样链上接 SoundTouch（变速不变调）。`swr_convert` 只改采样率会变调，不能直接做变速。本阶段不做，但 Resampler 是变速的天然落点（音频时间轴变换集中于此）。

---

## 边界（本文档不涉及）

- ❌ `swr_convert` 的具体调用参数（缓冲管理、延迟补偿）→ 实现阶段
- ❌ AudioResampledStore 的内部实现（与 AudioFrameStore 同模式）→ packet_queue.md / store 文档（待设计）
- ❌ open 时 miniaudio 能力探测的细节 → audio_sink.md（待设计）
- ❌ 变速不变调（SoundTouch）的具体接入 → 未来阶段