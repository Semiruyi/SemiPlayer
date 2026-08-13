# VideoSync 模块设计

> 视频管道的末端同步 worker。它读取播放时钟，从 `VideoRenderedStore` 选择当前应显示的帧，并在自己的工作线程同步调用宿主帧回调。

## 定位

当前阶段尚未接入字幕合成，因此数据流为：

```text
VideoRenderedStore(gen, CPU RGBA)
        |
        v
   [VideoSync] <----- AudioOutput.current_position()
        |
        +---- 同步调用 on_frame(const RenderedVideoFrame&)
                        |
                        +---- 宿主同步上传 GPU 或复制到自有内存
```

未来加入 Compositor 后，`VideoSync` 的输入可替换为最终合成帧 Store；选帧和回调契约不需要改变。

`VideoSync` 是拥有一个常驻 loop 线程的工作模块：

- `VideoRenderedStore` 是帧输入。
- 有音频时，`AudioOutput` 导出的播放位置是主时钟。
- 纯视频媒体使用内部 `steady_clock` 回退时钟。
- ApiLayer 通过 `configure/start_playback/pause_playback/unconfigure` 控制媒体会话和播放意图。
- 帧回调通过 `VideoSyncOptions` 传入，不存在独立的 `VideoOutput` 或 `VideoPresentationSink` 模块。

## 职责

- 丢弃非当前 generation 的帧。
- 等待音频播放位置建立，或为纯视频媒体维护本地单调时钟。
- 对未来帧等待到其 PTS；对已经到期的连续帧只交付最新一帧。
- pause 时停止常规换帧；暂停 seek 后允许为新 generation 交付一次目标附近的新帧。
- 在 `VideoSync` 工作线程同步调用已配置的帧回调。
- 观察输入 EOF，在最后一帧宿主回调返回后发布一次当前 generation 的 `VideoPlaybackFinished`。

不负责：

- 解码、像素格式转换或缩放；这些属于 VideoDecoder/VideoRenderer。
- GPU 上传或窗口绘制；这些属于宿主回调。
- 驱动音频时钟；VideoSync 只读 AudioOutput 的播放位置。
- 保存宿主配置；ApiLayer 保存会话前配置并在 `open` 时传入。
- 允许宿主跨回调持有帧；帧只在同步回调期间借用。

## 配置接口

```cpp
using VideoFramePresentationCallback =
    std::function<void(const RenderedVideoFrame&)>;

struct VideoSyncOptions {
    // 有音频流时为 true；纯视频媒体为 false。
    bool audio_master = true;

    // 空回调表示禁用宿主视频交付。
    VideoFramePresentationCallback on_frame;
};

class VideoSync {
public:
    virtual std::expected<void, VideoSyncError>
    configure(const VideoSyncOptions& options) = 0;

    virtual std::expected<void, VideoSyncError> start_playback() = 0;
    virtual std::expected<void, VideoSyncError> pause_playback() = 0;
    virtual void unconfigure() noexcept = 0;
};
```

配置来自 ApiLayer 的 `open` 编排：

- 输出格式和尺寸传给 `VideoRenderer::configure()`，不进入 VideoSync。
- 帧回调传给 `VideoSync::configure()`。
- 回调配置在一个媒体会话内保持不变。
- `on_frame` 为空时，VideoSync 仍消费到期帧，只是不调用宿主。

VideoSync 没有显式 `seek()`。generation 变化会唤醒 worker，清除 pending frame 和等待 deadline，并丢弃旧世代数据。暂停状态下的 generation 变化会允许新世代交付一帧，然后继续暂停。

## 选帧算法

### 有音频流

音频是主时钟。VideoSync 读取：

```text
AudioOutput.current_position()
    -> { generation, pts_us }
```

只有时钟 generation 与当前 generation 一致时才能用于选帧。若位置尚未建立，VideoSync 可以先保存一帧为 pending，但不能提前交付；它等待 `AudioPlaybackPositionReady` 或控制/generation 信号唤醒。

当当前时钟为 `current_pts` 时：

- `frame.pts <= current_pts`：帧已经到期。
- 连续存在多张到期帧：逐张消费，只保留最新到期帧作为本次交付候选；更旧的到期帧自然丢弃。
- `frame.pts > current_pts`：保存为 pending frame，并设置 `frame.pts - current_pts` 对应的等待 deadline。
- deadline 到达后回到循环顶部重新读取时钟和 generation，不盲目直接交付。
- 帧没有 PTS 时视为可立即交付，但仍受 generation、pause 和会话状态约束。

这种“排尽到期帧、只交付最新一帧”的策略同时完成追赶和丢帧，不需要额外维护固定的落后阈值。

音频可能早于视频结束。收到当前 generation 的 `AudioPlaybackFinished` 后，VideoSync 以最终音频位置为锚点切换到本地 `steady_clock`，继续调度视频尾帧，直到视频 EOF。这样应用层可以等待两条实际存在的管线都结束，而不会在音频 drain 完成时提前结束视频会话。

### 纯视频媒体

`audio_master == false` 时使用 `std::chrono::steady_clock`：

- 第一张带 PTS 的帧建立本地时钟锚点。
- `start_playback()` 恢复本地时钟推进。
- `pause_playback()` 冻结当前位置。
- generation 变化或 `unconfigure()` 重置本地时钟。

后续的到期判断与音频主时钟路径相同。

### 伪代码

```text
VideoSync loop:
    优先处理 configure/start/pause/unconfigure 控制命令

    若 generation 改变:
        清除 pending frame、deadline 和 EOF 状态
        切换 active generation
        paused_generation_pending = !playback_enabled

    若会话未配置:
        wait

    若已暂停且没有 paused_generation_pending:
        wait

    读取当前时钟
    若以音频为主且时钟尚未建立:
        最多保存一张当前 generation 帧为 pending
        wait AudioPlaybackPositionReady / generation / control

    candidate = 到期的 pending frame（若有）

    while Store 有当前 generation 数据:
        若是 EOF:
            标记 EOF；停止继续读取
        若帧尚未到期:
            保存为 pending，设置 deadline；break
        若帧已经到期:
            candidate = 该帧       # 覆盖旧 candidate，自动丢旧帧
            若暂停 seek 只需一帧: break

    若 candidate 存在:
        同步调用 on_frame(candidate)
        回调返回后销毁 candidate
        若是暂停 seek: 清除 paused_generation_pending

    若已观察到 EOF 且尚未通知:
        发布 VideoPlaybackFinished(active_generation)
```

## 回调契约

VideoSync 对内部回调的调用形式为：

```cpp
options_.on_frame(frame);
```

- 回调在 VideoSync 工作线程同步执行。
- `frame` 是只读借用，只在本次调用期间有效。
- 回调返回后 VideoSync 不保留帧，帧随当前处理步骤销毁。
- 回调为空时直接跳过调用。
- 回调异常必须在 worker 边界捕获，不能导致工作线程退出；公开 C ABI 同时规定宿主不得让异常跨越回调边界。
- 回调阻塞会直接阻塞后续视频同步，并可能让 `close`/`shutdown` 等待，因此宿主只能执行快速复制、上传或命令提交。

公开 `semi_video_frame_t` 的构造和借用规则见 [`../video_output/video_output.md`](../video_output/video_output.md)。VideoSync 本身只认识内部 `RenderedVideoFrame` 和内部回调类型，不依赖 C ABI 结构。

## 线程模型与唤醒

VideoSync 使用一个常驻 worker，不做固定周期轮询。唤醒源包括：

1. 控制命令：`configure/start_playback/pause_playback/unconfigure`。
2. `VideoRenderedStoreNotEmpty`：输入从空变为非空。
3. `GenerationChanged`：open/seek 产生新世代。
4. `AudioPlaybackPositionReady`：音频播放位置首次可用或需要重新评估。
5. `AudioPlaybackFinished`：音频先结束时切换到本地时钟继续调度视频尾帧。
6. pending future frame 的等待 deadline 到达。
7. 析构发出的 worker shutdown 请求。

Notifier 事件只作为唤醒 hint。worker 每次醒来都重新检查真实状态、当前 generation、Store 和时钟，不能假设某个事件仍然成立。

没有输入、没有 deadline、没有控制命令时使用无超时 `cv.wait()`；不增加周期性兜底唤醒来掩盖 lost wakeup。

## 状态

worker 生命周期与媒体会话状态分离。

```text
Worker:
Starting -> Alive -> ShuttingDown -> Stopped

Session:
Constructed -> Configuring -> Configured -> Unconfiguring -> Constructed
```

- worker 随模块构造启动，随模块析构退出。
- `configure()` 建立一个媒体会话，但默认不开始常规播放。
- `start_playback()` 允许连续选帧和交付。
- `pause_playback()` 冻结常规交付。
- `unconfigure()` 清除 pending frame、deadline、EOF、generation 和回调配置。

`close` 通过 ApiLayer 调用 `unconfigure()`。该调用与 worker 串行，并会等待正在执行的数据步骤及宿主回调返回，因此完成后不会再产生当前会话的新回调。

## 依赖

构造期注入：

| 依赖 | 用途 |
|------|------|
| `VideoRenderedSource` | 消费渲染完成的 CPU 视频帧和 EOF |
| `AudioOutput` | 有音频时读取当前播放位置 |
| `Notifier` | 订阅 Store、generation、音频位置/完成事件，并发布视频完成事件 |
| `Generation` | 判断当前数据世代 |

会话配置传入：

| 配置 | 用途 |
|------|------|
| `audio_master` | 选择音频主时钟或本地时钟 |
| `on_frame` | 同步交付宿主的内部回调 |

VideoSync 不构造或持有独立的宿主输出模块，不依赖 GPU API，也不参与 IoC 运行时服务定位。

## 边界与后续工作

- 字幕合成和最终帧 Store 尚未接入；当前直接消费 `VideoRenderedStore`。
- GPU texture、共享句柄和零拷贝输出属于另一种输出模式。
- 具体 GPU 上传、vsync 和窗口呈现策略属于宿主。
- 丢帧率、回调耗时和时钟异常的诊断统计可在后续增加，但不得改变同步回调的借用生命周期。
