# Player 生命周期（init / shutdown）

> **Player 是顶层根概念**，代表播放器整体。`init` / `shutdown` 是它的方法，**不属于任何模块**——它们操作的是"整个模块体系"的生灭，发生在模块体系存在之前 / 之后。
>
> 对比：`open`/`play`/`seek`/... 是 ApiLayer 的方法（命令路由），只在 init 之后、shutdown 之前可用。

## 定位

```
Player (顶层根, Dart 入口, init 前就存在)
├── init()         ← Player 自己: 装配整个模块体系(含 ApiLayer)
├── shutdown()     ← Player 自己: 拆除整个模块体系
└── [init 后才存在的模块体系]
    ├── ApiLayer   ← 普通模块, init 时被创建
    ├── IoCContainer
    ├── Demuxer / Decoder / Sink / ...
```

- **Player 是壳 + 引导者**：管模块体系的生灭。
- **ApiLayer 是被装配出的命令中枢**：init 之后才存在，处理 open/play/seek。
- **init 创建 ApiLayer，shutdown 销毁 ApiLayer** → ApiLayer 的方法只能在 init 后、shutdown 前调用。

## 装配时序

```
[进程启动]
   │
   │ Player.init()  (此时模块体系还不存在)
   │   → IoCContainer.assemble(): 按 DAG 拓扑创建所有模块 + 注入依赖
   │   → ApiLoop.spawn(): 启动命令执行线程
   │   → 模块体系就绪
   ▼
[模块体系运行: ApiLayer 接管, 处理 open/play/seek...]
   │
   │ Player.shutdown()  (模块体系仍在)
   │   → ApiLoop 退出
   │   → IoCContainer 逆序释放所有模块 (含 ApiLayer)
   │   → 模块体系消失
   ▼
[进程可退出, 或再次 init]
```

---

## init

```
Player::init():
    if initialized: return Ok(())        // 幂等
    
    // 唯一职责: IoC 装配 (DAG 拓扑创建 + 依赖注入) + 启动 ApiLoop
    // ★ 不碰任何技术细节 (FFmpeg/miniaudio 等是各模块内部职责, init 不可见)
    ioc = IoCContainer::assemble()
    api_loop = ApiLoop::new(ioc, command_queue)
    api_loop.spawn()
    
    initialized = true
    Ok(())
```

### init 的职责

- **IoC 装配**：按 DAG 拓扑顺序创建所有模块、构造时注入 `std::shared_ptr<依赖>`。模块各自的技术初始化（FFmpeg 注册、miniaudio host 获取等）在模块 `constructor` 内部完成，**init 不知道这些**。
- **启动 ApiLoop**：命令执行线程跑起来。

### init 不做什么

- ❌ 不做 FFmpeg/miniaudio 等具体技术初始化（各模块内部职责）
- ❌ 不打开任何媒体（那是 open）
- ❌ 不绑定媒体流（那是 open 的 decoder.configure）
- ❌ 不启动 demuxer/decoder 读数据（那是 play）
- ❌ 不创建 miniaudio 流 / 解码器上下文（按媒体特性，open 时建）

### 关键原则：init 不懂技术细节

init 只知道"有这些模块类型、按依赖装配"，不知道模块内部用 FFmpeg 还是别的。换底层库不用改 init。这是**依赖倒置**——引导层只懂装配，不懂实现。

模块初始化的全局协调（如 miniaudio 进程级单例）由模块内部局部静态（Meyers singleton）或共享注入解决，不归 init。

---

## shutdown

```
Player::shutdown():
    if !initialized: return Ok(())       // 幂等
    
    // 逆序释放: 先停 ApiLoop, 再逆序释放模块
    api_loop.stop()                      // ApiLoop 退出, 停止处理命令
    ioc.dispose()                        // 逆序释放所有模块 (各模块析构自管资源回收)
    
    initialized = false
    Ok(())
```

### shutdown 的职责

- **停 ApiLoop**：命令执行线程退出。
- **逆序释放模块**：IoCContainer 按装配的逆序释放各模块。各模块的析构自管资源回收（关 miniaudio 流、销毁解码器、关文件等），shutdown 不懂这些细节。

### shutdown 不做什么

- ❌ 不逐个调模块的清理方法（那是模块析构的事）
- ❌ 不懂 FFmpeg/miniaudio 的回收（模块内部）

### shutdown 是命令队列的最后一条命令

shutdown 本身可作为命令投递（走命令队列），它是队列的**最后一条**——执行完 ApiLoop 退出。shutdown 后再调任何命令 → 错误（"not initialized"）。

---

## 状态机（lifecycle 层）

```
Uninitialized ──init()──▶ Idle (模块体系已装配, ApiLoop 运行, 无媒体)
               ◀──────────
                shutdown()
```

- **Uninitialized**：模块体系不存在。`init` 之前 / `shutdown` 之后。此态调任何命令都报错。
- **Idle**：模块体系就绪（ApiLayer 可用），无媒体。此态可 `open`。

> 注：`Idle`/`Ready`/`Playing` 等播放会话状态由 ApiLayer 维护（见 api_layer.md）。`Uninitialized` 是 lifecycle 层的状态——它表示"模块体系是否存在"，比 ApiLayer 的会话状态更外层。

---

## 与各层的关系

| 层 | 职责 | 状态 |
|----|------|------|
| **Player (lifecycle)** | 模块体系生灭（init/shutdown）| Uninitialized / Idle |
| **ApiLayer** | 命令路由 + 会话状态 | Idle / Ready / Playing / Paused / Ended |
| **IoCContainer** | 装配/释放工具，被 Player 使用 | — |
| **各业务模块** | 各自功能 + 内部技术初始化/回收 | 各自内部状态 |

---

## 边界（本文档不涉及）

- ❌ IoC 装配的具体模块清单与拓扑 → architecture.md
- ❌ 各模块内部技术初始化（FFmpeg/miniaudio）→ 各模块文档
- ❌ open/play/seek 等命令编排 → api_layer/
- ❌ close（媒体资源回收，非模块体系拆除）→ api_layer/close.md（待设计）
