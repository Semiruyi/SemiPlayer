# 错误与结果约定

> 分层：Api / C ABI 统一错误码；内部模块默认 `bool`；特殊多结果用模块私有 enum。

## 原则

| 层 | 怎么表达 |
|----|----------|
| **ApiLayer / C ABI / 命令句柄 resolve** | 统一 `semi_status`（见 `include/semi_player/status.h`） |
| **内部模块同步调用** | 默认 `bool`：`true` = 成功，`false` = 失败 |
| **多种合法结果（含非错误）** | 模块私有 `enum class`（如 `semi::log::InitResult`） |
| **异步 / 跨线程失败** | 日志 + `Notifier` 通知；不硬塞返回值链路 |

## 细则

### 1. 统一错误只出边界

- 进程外（Dart / C ABI）与 ApiLayer 对外结果**只认** `semi_status`。
- 内部模块**不**传播整表错误码，避免全链路 map。
- 模块私有 enum **不**进入全局表；若调用方超过本模块，优先收成 `bool` 或上浮到 Api 时再映射。

### 2. `bool` 约定

- `true` = 成功，`false` = 失败。
- 建议 `[[nodiscard]]`，避免忽略失败。
- 失败细节：打日志；需要上层感知时由 **ApiLayer 映射**为 `semi_status`，或发 Notifier（异步路径）。

### 3. 模块私有 enum

适合：

- 多种非错误结果（`Ready` / `ConsoleOnly` / `AlreadyInitialized`）
- 调用方必须分支的局部状态（如 `PushResult::Ok | Full`）

不适合：跨多个无关模块传递的同一种失败（应收口到 Api 统一码或 Notifier）。

### 4. 生命周期（IoC / init / shutdown）

- `assemble` / `dispose` 返回 `bool`（内部约定）。
- **幂等视为成功**（已装配再 assemble、未装配再 dispose → `true`），与 `docs/lifecycle.md` 一致。
- 真失败（如未来某模块构造失败）→ `false`，C ABI 映射为 `SEMI_ERR_ASSEMBLE_FAILED` 等。
- `semi_player_init` / `semi_player_shutdown` 返回 `semi_status`（`int`，`SEMI_OK == 0`）。

### 5. 命令句柄（后续）

- 控制命令仍立即返回 handle；**完成结果**用同一套 `semi_status`（或带 payload 的 Result，status 字段共用）。
- 非法状态（未 open 就 play）→ `SEMI_ERR_INVALID_STATE`。
- 未 init → `SEMI_ERR_NOT_INITIALIZED`。

## 新增错误码

优先复用 `status.h` 已有项；只有 **Api 边界需要调用方分支** 时才加码，避免表膨胀。
