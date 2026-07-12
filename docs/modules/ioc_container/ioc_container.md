# IoCContainer 模块设计

> 基础设施模块。**装配器**——按 DAG 拓扑顺序构造所有模块、注入依赖,逆序释放。运行时透明(只管生灭,不参与命令/通知/数据流)。
> 被 `Player::init` / `Player::shutdown` 调用(见 lifecycle.md)。

## Context

28 个模块,依赖关系在 architecture.md 已明确画成 DAG(5 层拓扑)。这种"模块数可控、依赖静态已知"的场景,**不需要运行时服务定位 / 自动注入框架**,手动装配最合适:

| 方案 | 适合 | 本项目 |
|------|------|--------|
| 自动注入框架(注册+反射/trait object 查找) | 模块多、依赖动态、跨 crate 插件 | ❌ 过度设计 |
| **手动装配(按拓扑顺序 new + 注入 Arc)** | 模块数可控、依赖静态已知 | ✅ 28 个模块,DAG 已定 |

手动装配的好处:① 编译期就能查依赖(Rust 类型系统帮你验证——传错 Arc 类型编译不过);② 装配顺序肉眼可见,不用追框架;③ 无运行时开销;④ 清晰直白比花哨框架更显功底。

---

## 定位

```
Player::init()  ──→  IoCContainer::assemble()  ──→  按 DAG 拓扑构造所有模块 + 注入 Arc<依赖>
                                                              │
                                                              ▼
                                                  装配完成,持有所有模块 Arc
                                                  ApiLayer/ApiLoop 拿到所需 Arc 自持
                                                              │
                                              [运行时: IoC 透明,只保活,不被访问]
                                                              │
Player::shutdown() ──→ IoCContainer::dispose() ──→ 手动逆序释放(先停依赖者,后释放被依赖者)
```

- **基础设施层,第 0 层**(无依赖,被 Player 使用)。
- **纯装配器**:只管模块体系的生灭,不参与运行时任何业务。
- **生命周期**:init 时 assemble 创建,shutdown 时 dispose 释放。整个播放器运行期间常驻。

---

## 职责

只做两件事:

1. **装配(assemble)**:按 DAG 拓扑顺序逐个 `new` 模块,构造时注入 `Arc<依赖>`,最终持有所有模块的 Arc。装配后把工作模块 Arc 分发给 ApiLayer/ApiLoop(它们自持,运行时不再访问 IoC)。
2. **释放(dispose)**:**手动逆序**释放所有模块(被依赖者后释放),让各模块 Drop 自管资源回收。释放前可插入 stop 逻辑(如先停 ApiLoop、停工作线程再 drop)。

**不做的**(明确边界,防过度设计):
- ❌ 不提供运行时服务定位(不按类型查找模块)——装配后 Arc 直接持有,不用查。
- ❌ 不做自动依赖注入——依赖关系写在每个模块 `new()` 签名里,装配代码手动传。
- ❌ 不管理模块内部状态——模块自管,IoC 只管生灭。
- ❌ 不参与命令/通知/数据流——运行时透明。
- ❌ 不并发装配——收益≈0,代价大(详见设计决策)。

---

## 对外接口

```rust
pub struct IoCContainer {
    // 持有所有模块的 Arc(装配后存在,dispose 后清空)
    // 字段声明顺序 = 装配顺序(第0层先声明,接口层后声明)
    // 注意:dispose 是手动逆序 drop,不依赖字段声明顺序(见下)
    generation: Arc<Generation>,
    notifier: Arc<Notifier>,
    gpu_device: Arc<dyn GpuDevice>,
    command_queue: Arc<CommandQueue>,
    video_packet_queue: Arc<VideoPacketQueue>,
    // ... 各 PacketQueue / Store
    audio_clock: Arc<AudioClock>,
    demuxer: Arc<Demuxer>,
    video_decoder: Arc<VideoDecoder>,
    // ... 各工作模块
    api_loop: Arc<ApiLoop>,
    api_layer: Arc<ApiLayer>,
}

impl IoCContainer {
    /// 装配:按 DAG 拓扑顺序构造所有模块,注入依赖
    /// 无参——装配是确定性的(模块清单+依赖固定),平台差异(cfg)在内部处理
    pub fn assemble() -> Self {
        // 第0层:无依赖的先构造
        let generation = Arc::new(Generation::new());
        let notifier = Arc::new(Notifier::new());
        let gpu_device: Arc<dyn GpuDevice> = Arc::new(D3D11GpuDevice::new()); // 按平台 cfg 选实现
        let command_queue = Arc::new(CommandQueue::new());
        let video_packet_queue = Arc::new(VideoPacketQueue::new());
        // ... 各 PacketQueue / Store / AudioClock

        // 第1层:注入第0层
        let demuxer = Arc::new(Demuxer::new(
            video_packet_queue.clone(), audio_packet_queue.clone(), subtitle_packet_queue.clone(),
            generation.clone(), notifier.clone(),
        ));
        let video_decoder = Arc::new(FfmpegVideoDecoder::new(
            video_packet_queue.clone(), video_frame_store.clone(),
            generation.clone(), gpu_device.clone(), notifier.clone(),
        ));
        // ... 各 decoder / resampler

        // 第2-4层:逐层注入
        // ... video_renderer / subtitle_renderer / compositor / video_sync / audio_sink / progress_reporter

        // 第5层 + 接口层
        let api_loop = Arc::new(ApiLoop::new(command_queue.clone(), /* 各工作模块 Arc */));
        let api_layer = Arc::new(ApiLayer::new(api_loop.clone(), /* 各工作模块 Arc */));

        Self { generation, notifier, gpu_device, /* ... */ api_loop, api_layer }
    }

    /// 释放:手动逆序 drop(先停依赖者,后释放被依赖者)
    /// 显式写释放顺序,不靠字段声明顺序的隐式约定
    pub fn dispose(self) {
        // 先停接口层(它持有工作模块 Arc,先释放才能让工作模块 Arc 计数归零)
        drop(self.api_layer);
        drop(self.api_loop);

        // 第4层 → 第0层 逆序
        drop(self.video_sync);
        drop(self.audio_sink);
        drop(self.progress_reporter);
        drop(self.compositor);
        drop(self.video_renderer);
        drop(self.subtitle_renderer);
        drop(self.demuxer);
        drop(self.video_decoder);
        drop(self.audio_decoder);
        drop(self.audio_resampler);
        drop(self.subtitle_decoder);
        // 第0层最后
        drop(self.audio_clock);
        // ... 各 Store / PacketQueue
        drop(self.gpu_device);
        drop(self.command_queue);
        drop(self.notifier);
        drop(self.generation);
    }
}
```


---

## DAG 装配顺序(对应 architecture.md 5 层拓扑)

```
第0层: Generation, CommandQueue, Notifier, GpuDevice,
       VideoPacketQueue, AudioPacketQueue, SubtitlePacketQueue,
       VideoFrameStore, AudioFrameStore, AudioResampledStore,
       VideoRenderedStore, SubtitleFrameStore, FinalFrameStore, AudioClock
第1层: Demuxer, VideoDecoder, AudioDecoder, AudioResampler, SubtitleDecoder
第2层: VideoRenderer, SubtitleRenderer
第3层: Compositor
第4层: VideoSync, AudioSink, ProgressReporter
第5层: ApiLoop
接口层: ApiLayer(持有 ApiLoop + 各工作模块 Arc)
```

**装配顺序** = 从第0层到接口层(从无依赖到有依赖)。**释放顺序** = 严格逆序(接口层先,第0层最后)。

---

## 关键设计决策

### 手动装配,不用自动注入框架
28 个模块、依赖静态已知、DAG 已定,手动装配够用。自动注入框架(注册+查找)是为"模块多/动态/插件化"设计的,本项目用不上,引入只增加复杂度。手动装配让依赖关系写在装配代码里,一眼看清,编译期类型系统验证(传错 Arc 类型编译不过)。

### dispose 手动逆序释放,不靠 Rust 自动 drop
Rust struct 字段按声明顺序逆序自动 drop,看似能自动实现逆序释放,但有**静默的正确性风险**:
- Arc 是引用计数,模块真正 drop 要等所有 Arc clone 都没了。
- IoC 持有各模块 Arc,ApiLayer 也持有工作模块 Arc 的 clone。
- 若靠字段声明顺序,一旦顺序写错(被依赖的字段声明在依赖者之后),编译器不报错,运行时可能 use-after-free(被依赖者先没了,依赖者还在用)。

**手动逆序 drop 的优势**:
- **意图显式**:释放顺序写在代码里,一眼看清,不靠隐式约定。
- **顺序可控**:不依赖 Rust 自动 drop 规则,想怎么排就怎么排。
- **可加释放逻辑**:某模块释放前要"优雅停止"(如先停线程再 drop),手动释放能在 drop 前插入 stop 调用(如 `api_loop.stop()` 后再 drop)。
- **配合 lifecycle.md**:lifecycle.md 说"ApiLoop 退出 → IoC 逆序释放模块",手动释放正好对应这个编排。

**释放顺序关键**:ApiLayer/ApiLoop 最先释放(它们持有工作模块 Arc,先释放才能让工作模块 Arc 计数归零,工作模块才能真正 drop)。

### 不并发装配(收益≈0,代价大)
同一层并发装配的收益分析:
- **真实瓶颈只有一个**:GpuDevice 建 D3D11 device(~10-50ms),其它 27 个模块装配都是微秒级。
- 并发第0层(14 模块):GpuDevice 仍是瓶颈,并发其它 13 个不省时间,**收益接近 0**。
- 并发第1层(5 decoder):本来就 <1ms,**收益 0**。

并发装配的代价:
- 复杂度大增(并发 new + Arc 注入,要处理线程安全、JoinHandle、错误传播)。
- 装配代码从"顺序直白"变"并发协调",可读性骤降。
- 同层装配顺序不确定,若有隐式顺序依赖会出 bug。
- init 是一次性开销(非热路径),省 10-50ms 用户无感。

**结论**:不并发。如果要优化 GpuDevice 耗时,应优化 GpuDevice 自身(懒加载,见下),而非并发装配。

### GpuDevice 懒加载(消除装配期唯一瓶颈)
GpuDevice 的 `new()` 可做成轻量(只记配置,不立即建 D3D11 device),真正建 device 推迟到第一次 `acquire_buffer`/`device_handle` 调用时(懒加载)。这样:
- 装配期所有模块都微秒级,串行也无感。
- D3D11 device 建在第一次硬解需要时,符合"按需初始化"。
- dispose 时 GpuDevice drop 自动释放 device。
- 串行装配更无可挑剔——连唯一瓶颈都消除了。

> 这是 GpuDevice 实现细节,归 GpuDevice 文档/实现阶段。IoC 这里只受益:装配期无重模块。

### 装配后 Arc 分发自持,运行时不访问 IoC
装配时 ApiLayer/ApiLoop 就拿到所需模块 Arc 的 clone 并自持。运行时调命令直接用自持的 Arc,不经过 IoC 查找。IoC 的唯一运行时作用是"持有所有 Arc 让模块保活直到 dispose"。这符合现有 DI 设计(构造期注入,运行时直接持有,不查容器),IoC 运行时透明。

---

## 与现有文档的一致性

- **lifecycle.md**:`Player::init` 调 `IoCContainer::assemble()`,`shutdown` 调 `dispose()`——接口对得上。lifecycle.md 说"ApiLoop 退出 → IoC 逆序释放模块",dispose 手动逆序正好对应。
- **architecture.md**:"init 时按 DAG 拓扑顺序构造所有模块、构造时注入 Arc<依赖>;shutdown 时逆序释放"——职责对得上。
- **各模块 `new()` 签名**:暴露依赖(显式可见),装配代码把这些串起来。依赖关系在 `new()` 签名里,装配代码是"把这些签名串起来"的胶水。

---

## 坑与边界

### dispose 顺序与 Arc 引用计数
Arc 模块真正 drop 要等所有 Arc clone 都没了。dispose 手动逆序 drop IoC 持有的 Arc,但 ApiLayer 等也持有 clone——必须先 drop ApiLayer(它的 clone 没了),工作模块的 Arc 计数才能归零真正 drop。所以 dispose 顺序里 ApiLayer/ApiLoop 必须最先。这是手动逆序释放的核心约束。

### 装配失败的错误处理
某模块 `new()` 失败(如 GpuDevice 建 device 失败)怎么办?assemble 返回 `Result<Self, Error>`,失败时已构造的模块要回滚(逆序 drop 已建的部分)。这是实现细节,文档标"装配失败需回滚已构造模块"。

### 平台差异(cfg)
GpuDevice 的具体实现按平台 cfg 选(Windows → D3D11GpuDevice)。assemble 内部用 cfg 分支,对外接口不变(都返回 `Arc<dyn GpuDevice>`)。其它模块无平台差异。

### 字段声明顺序
虽然 dispose 是手动逆序(不依赖字段声明顺序),但 struct 字段声明建议按装配顺序(第0层先),便于阅读和与 assemble 代码对应。这不影响释放(手动 drop 顺序由 dispose 函数决定)。

---

## 边界（本文档不涉及）

- ❌ 各模块 `new()` 的具体签名 → 各模块文档
- ❌ 装配失败的具体回滚实现 → 实现阶段
- ❌ GpuDevice 懒加载的具体实现 → GpuDevice 文档 / 实现阶段
- ❌ IoC 是否需要暴露模块访问器(getter)→ 本设计选"装配后分发自持,运行时不访问 IoC",不需要 getter
        drop(self.video_sync);
        drop(self.audio_sink);
        drop(self.progress_reporter);
        drop(self.compositor);
        drop(self.video_renderer);
        drop(self.subtitle_renderer);
        drop(self.demuxer);
        drop(self.video_decoder);
        drop(self.audio_decoder);
        drop(self.audio_resampler);
        drop(self.subtitle_decoder);
        // 第0层最后
        drop(self.audio_clock);
        // ... 各 Store / PacketQueue
        drop(self.gpu_device);
        drop(self.command_queue);
        drop(self.notifier);
        drop(self.generation);
    }
}
```

### 接口要点

1. **`assemble()` 无参,返回 Self**:装配是确定性的(模块清单+依赖固定),不需要外部传配置。平台差异(如选哪个 GpuDevice 实现)在内部用 cfg 处理。
2. **`dispose(self)` 消费 self**:拿走所有权,手动逆序 drop 各字段。**显式写释放顺序**,不依赖 Rust 自动 drop(见设计决策)。
3. **持有所有模块 Arc**:装配后 IoC 持有一切保活;ApiLayer/ApiLoop 在装配时就拿到所需 Arc 的 clone 并自持,运行时不再访问 IoC。IoC 的唯一运行时作用是"持有 Arc 让模块保活直到 dispose"。

