# RealTimeNotifier 模块设计

`RealTimeNotifier` 是普通 `Notifier` 之外的实时通知机制，用于音频设备回调等不能阻塞的发布路径。

## 边界

- 普通 `Notifier`：运行时订阅、类型擦除、同步回调，适合控制面和低频业务事件。
- `RealTimeNotifier`：事件类型、每类最大 sink 数在编译期声明；运行期拓扑固定，适合实时数据面。

它不感知任何业务事件语义。播放器在装配处定义具体实例，例如
`AudioOutputRealTimeNotifier`，再把确认消费帧数等事件加入其中。

## 发布路径

`notify(event)` 按事件类型路由到对应的固定 sink 数组。该路径不加锁、不分配、不等待、不记录日志，也不捕获异常。所有 sink 回调必须 `noexcept`，并且自身满足发布方的实时约束。

**同一个 `RealTimeNotifier` 实例的所有接口都不能并发调用。** 这包括两个 `notify()` 调用之间，以及 `notify()` 与任一控制面接口之间。每个实例只服务一个发布线程；需要多个实时生产者时，为每个生产者创建独立实例。

## 生命周期

```text
Idle -- register_sink --> Idle -- seal --> Sealed -- notify --> Sealed
  ^                                                   |
  +------ unseal (发布线程已停止且 join) -------------+
```

- `register_sink()` / `unregister_sink()` 只允许在 `Idle` 调用。
- 所有控制面调用也必须彼此串行，且不得与 `notify()` 并发。
- `seal()` 必须先于音频设备回调启动。
- 调用 `unseal()` 前，调用方必须停止并 join 所有可能调用 `notify()` 的线程。
- sink 必须在从 notifier 注销后才能销毁。

这些限制由调用方维护。违反时可能产生悬垂指针或数据竞争，因此 Debug 构建会断言，Release 构建返回失败而不修改拓扑。

## 当前音频链路

```text
miniaudio callback
  -> 确认消费帧数（`std::uint32_t`）
  -> AudioOutputRealTimeNotifier
  -> DefaultAudioOutput::ProgressSink
  -> active_generation
  -> AudioOutput 内部 AudioPlaybackClockState
  -> 唤醒填充工作线程
```

只有真正从 PCM ring 读到的媒体帧会产生确认消费帧数通知；补出的静音不会推进时钟。
