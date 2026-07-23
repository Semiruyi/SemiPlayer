# ApiLayer 模块设计

`ApiLayer` 是应用层的具体命令中枢，由 IoC 在 `init` 时创建。它拥有命令队列、任务表和唯一命令线程；不再拆分独立的 ApiLoop 或 CommandQueue 模块。

## 对外模型

控制接口 `open/play/pause/seek/close/set_volume` 只负责构造任务并立即返回非零 `CommandHandle`。`0` 表示 ApiLayer 未运行、任务容量已满或入队失败。

宿主对 handle 可调用：

- `await(handle, out_result)`：阻塞到终态，复制最终结果并消费 handle。返回命令的 `semi_status_t`；成功 open 的 `MediaInfo` 在 `out_result` 中。资源无法打开或探测时返回 `SEMI_ERR_INVALID_RESOURCE`，内部失败返回 `SEMI_ERR_INTERNAL`。
- `cancel(handle)`：仅接受尚未开始任务的取消请求。任务不从队列移除，仍由命令线程取出并完成为 `SEMI_ERR_CANCELLED`。

没有 `release`、独立 `get_media_info` 或会话查询接口。`await` 是唯一的结果读取与正常回收路径。

## 任务状态

```text
Queued -> Running -> Completed
   |
   +-> CancelRequested -> Cancelled
```

- `cancel` 只把 `Queued` 标为 `CancelRequested`，返回值表示请求是否被接受。
- 命令线程是唯一写入 `Completed` / `Cancelled`、结果和条件变量通知的一方。
- 已经 `Running` 的命令不支持中断；`cancel` 返回 `false`。
- `await` 可有多个竞争调用者，但仅一个调用者能消费结果；其余调用得到 `SEMI_ERR_INVALID_HANDLE`。

## 并发与容量

任务表、队列和 handle 分配由同一把调度锁保护；每个任务另有互斥锁和条件变量，供 `await` 等待终态。等待前会先取得任务的 `shared_ptr`，不会持有调度锁阻塞。

当前内部固定最多保留 1024 个排队、执行中和未消费完成任务。新任务入队前会按完成顺序淘汰最早的未消费终态任务；若剩余任务都在排队或执行，则新命令返回 `0`。被淘汰 handle 后续 `await` 返回 `SEMI_ERR_INVALID_HANDLE`。

## 执行边界

当前只实现命令基础设施，媒体业务模块和会话状态机尚未接入。命令线程仍会把每个任务完成并通知 `await`，但控制命令统一返回 `SEMI_ERR_INTERNAL`，不会被伪装为成功。后续按模块实现进度直接在 ApiLayer 命令分发中加入实际操作。

## 生命周期

`start()` 创建命令线程并开始接收任务。`stop()` 停止接收新任务，等待当前任务结束，并让命令线程将所有未开始任务逐一完成为取消状态后退出。每个已返回的 handle 都因此有终态通知机会。
