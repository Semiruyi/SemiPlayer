# Notifier 模块设计

> 基础设施模块。**通知中心**——模块注册感兴趣的通知、状态变化方发送通知，纯机制，无业务语义。
> 所有跨模块的"状态变化通知"（队列状态、EOF、时钟跳点、错误等）都流经它。

## 定位

| 维度 | 内容 |
|------|------|
| 层 | 基础设施层 |
| 是否模块 | ✅ 是（基础设施模块）|
| **业务语义** | ❌ **无**——不认识 VideoQueueNotFull / Eof / ClockJumped 是什么意思 |
| 线程 | 无（被各线程调用）|
| 职责 | 提供通知中心机制：按**通知类型**注册回调、按**通知类型**分发通知 |
| 实例 | 一个全局实例，`Arc<Notifier>` 注入给所有需要发/收通知的模块 |

**核心**：Notifier 是个**通知中心**。它只懂"某个通知类型 T 的注册者列表"，不懂 T 的业务含义。业务模块定义 T、send T、register T。

---

## 提供的两个能力

```
Notifier:
    register<T>(callback)    // 注册通知: "T 类型通知发生时, 调这个回调"
    send<T>(event: &T)       // 发送通知: "T 类型通知发生了", 唤醒所有注册者
```

- `T` 是通知类型（Rust 里是个 struct，**业务模块定义**，见下）。
- 内部按 `TypeId`（T 的类型标识）维护 `HashMap<TypeId, Vec<回调>>`。
- `send` 时按 T 的 TypeId 找注册列表，逐个调用回调。

---

## 通知类型由谁定义（关键原则）

> **通知类型由发送方定义**，接收方引用。Notifier 不定义任何通知类型。

### 为什么是发送方定义

| 归属 | 依赖方向 | 循环风险 | 评价 |
|------|---------|---------|------|
| **发送方定义**（采用）| 接收方 → 发送方（顺向，接收方本就消费发送方数据）| 无 | ✅ 推荐 |
| 接收方定义 | 发送方 → 接收方（反向，破坏 DAG）| **有**（资源管理者→工作模块，且工作模块→资源管理者消费，成环）| ❌ |
| Notifier 定义 | 所有模块 → Notifier 的业务知识 | 无（但中心耦合）| ❌（Notifier 染业务语义）|

**发送方定义不引入新依赖**：接收方本来就依赖发送方（消费它的数据/状态），引用发送方的通知类型是**顺向**的，零额外依赖，不破坏 DAG。

### 不循环的证明（具体例子）

```
VideoPacketQueue (第0层) 定义 VideoQueueNotEmpty / VideoQueueNotFull
VideoDecoder (第1层): 依赖 VideoPacketQueue (消费包 + 引用通知类型注册)  ← 顺向 第1→第0 ✓

AudioClock (第0层) 定义 ClockJumped
VideoSync (第1层): 依赖 AudioClock (读时钟 + 引用 ClockJumped 注册)  ← 顺向 第1→第0 ✓

Demuxer (第1层) 定义 Eof / DemuxError
ApiLayer (第3层): 依赖 Demuxer (调方法 + 引用 Eof 注册)  ← 顺向 第3→第1 ✓
```

所有"接收方引用发送方通知类型"都是 DAG 上的顺向路径，不构成环。

---

## 通知类型示例（各业务模块自定义）

Notifier **不内置任何通知类型**。所有通知类型由业务模块自己定义：

```rust
// 在 VideoPacketQueue 模块里 (它是发送方)
pub struct VideoQueueNotEmpty;
pub struct VideoQueueNotFull;

// 在 AudioPacketQueue 模块里
pub struct AudioQueueNotEmpty;
pub struct AudioQueueNotFull;

// 在 Demuxer 模块里
pub struct Eof;
pub struct DemuxError(pub String);        // ★ 通知可携带数据

// 在 AudioClock 模块里
pub struct ClockJumped { pub pts: i64 }   // ★ 通知可携带数据

// 在 VideoFrameStore 模块里
pub struct VideoFrameReady;
```

业务模块**通过定义通知类型来声明自己的通知接口**。Notifier 只搬运，不认识这些类型。

---

## 回调能读通知携带的数据（重要）

回调签名是 `Fn(&T)`——**传入通知引用**，回调可读通知的数据字段：

```rust
// 通知携带数据
struct ClockJumped { pts: i64 }
struct DemuxError(String)

// 回调能读数据
notifier.register::<ClockJumped>(|event| {
    self.target_pts.store(event.pts)   // 读 event.pts
    self.cv.notify_one()               // 轻量: 设标志 + notify 自己的 cv
})
notifier.send(&ClockJumped { pts: 1000 })
```

通知是 struct，可携带任意数据字段（pts、错误信息、帧信息等），回调通过 `&T` 读取。

---

## 同步回调（已定）

- **send 在发送方线程同步调用回调**——不排队、无独立通知线程、无延迟。
- 回调在 send 调用方的线程上执行。例：Demuxer（demuxer 线程）send(Eof) → 在 demuxer 线程调 ApiLayer 的回调。
- **回调必须极轻量**——只做"读通知数据 + 设标志 + notify 自己的 cv"，不拿重锁、不干重活、不阻塞。
- **回调里不能反向等待发送方**（防死锁）——比如回调里不能去 join 发送方线程。

---

## 接口草案（Rust）

```rust
pub struct Notifier {
    // TypeId -> 该通知类型的回调列表 (异构存储, 按 TypeId 取回)
    slots: Mutex<HashMap<TypeId, Box<dyn Any + Send>>>,
}

impl Notifier {
    /// 注册通知: T 类型通知发生时调 cb。cb 接收 &T, 可读通知数据。
    pub fn register<T: 'static>(&self, cb: impl Fn(&T) + Send + Sync + 'static)

    /// 发送通知: 同步在当前线程逐个调 T 的注册回调。
    pub fn send<T: 'static>(&self, event: &T)
}
```

（内部用 `Any` 存异构回调列表、按 TypeId 取回，是 Rust 类型化通知中心的标准实现模式。）

---

## 依赖关系

- **Notifier 依赖谁**：无（纯基础设施，第 0 层）。
- **谁依赖 Notifier**：所有需要发/收通知的业务模块（VideoPacketQueue / AudioPacketQueue / VideoFrameStore / Demuxer / AudioClock / VideoDecoder / AudioDecoder / VideoSync / ApiLayer ...）。
- Notifier 是 DAG 里被广泛依赖的第 0 层节点。

---

## Notifier 不做什么（边界）

- ❌ **不定义任何业务通知类型**（业务模块定义）。
- ❌ **不知道通知含义**（不懂 VideoQueueNotEmpty 是"视频队列非满"）。
- ❌ **不阻塞、不 wait**（只调回调；阻塞是工作模块自己 cv 的事）。
- ❌ **不持有业务模块的引用**（回调是匿名的，注册方自己传入）。
- ❌ **不异步排队**（同步调回调，无独立通知线程）。

---

## 关键设计决策

### Notifier 无业务语义（基础设施原则）
Notifier 是纯机制（register/send + 按类型分发），不认识任何播放器业务概念。这是它**通用、可复用、可换实现**的根基。换项目能直接复用 Notifier；业务演化（加通知）不改 Notifier。

### 通知类型发送方定义（防循环 + 业务自治）
- **发送方最清楚自己发什么**，自己定义通知类型，业务自治。
- 接收方引用发送方通知类型是**顺向依赖**（本就消费发送方数据），不破坏 DAG、不循环。
- 通知和产生者**语义内聚**（VideoQueueNotEmpty 就在 VideoPacketQueue 旁边定义）。

### 通知类型可携带数据
通知是 struct，可带任意字段（pts、错误信息等）。回调 `Fn(&T)` 传入通知引用，可读数据。这让通知不只是"发生了"，还能传达"发生了什么"。

### 同步调回调
send 在当前线程同步调回调。简单、无延迟。代价：回调须轻量、不能反向等发送方（防死锁）。我们的回调都是轻量（notify cv），同步合适。

### 一个全局实例
所有通知类型流经同一个 Notifier，按 TypeId 分流。Arc 注入给所有模块。不碎片化（无需每模块一个 Notifier）。

---

## 备选方案（仅当出现真循环时用）

如果某个通知出现"接收方在 DAG 上够不到发送方"的真循环（接收通知会引入反向依赖），解法：把**该个别通知类型**提到一个中立的 `events` / `types` 数据模块，发送方和接收方都依赖它，互不依赖。

```
events 模块 (中立数据定义处, 大家都依赖):
    pub struct SomeEvent { ... }
发送方: use events::SomeEvent; send(it)
接收方: use events::SomeEvent; register(it)
```

**注意**：
- 这是**备选**，不是默认。先用"发送方定义"，遇到真循环再提取。
- 只提取**有循环问题的个别通知**，不是把所有通知都搬过去。
- 提取到**独立的 events 数据模块**，不是塞进 Notifier（Notifier 仍保持纯机制）。

---

## 边界（本文档不涉及）

- ❌ 各业务通知类型的具体定义（VideoQueueNotEmpty / Eof / ClockJumped 等）→ 各业务模块文档
- ❌ 工作模块如何用 Notifier 唤醒自己的 cv → 各工作模块文档（如 demuxer.md）
- ❌ TypeId 异构存储的具体实现细节 → 实现阶段
