# 性能统计

`infrastructure/statistics/PerformanceStatistics` 订阅模块统计快照，通过独立线程写 JSON Lines 文件。
由 IoC 装配，先于 Worker 创建、晚于 Worker 释放；没有新增公开 C API。
当前接入视频解码器，其他模块可后续定义自身快照并增加订阅。

## 输出

默认开启，文件为当前工作目录下 `logs/performance/statistics-<Unix 纳秒>-<序号>.jsonl`。
每次 IoC 装配创建独立写入器，仅在收到报告后创建文件；普通日志会记录实际输出路径。
同一文件可包含多次 Open 对应的报告。每个报告独占一行，`schema_version=1`。
不使用 session_id，配置开始和统计结束的 Unix 毫秒时间用于定位报告。

视频解码器从成功 configure 后开始累计，在 unconfigure 前提交。
自然 EOF 仅 drain，不结束配置周期：播放结束后仍可 Seek，因此报告在 Close 或取消配置时产生。
重复 unconfigure 不重复提交；错误时立即提交 `backend_failure`，销毁仍配置的模块时提交 `shutdown`。
配置失败不生成报告；重新配置仍遵守既有约定，必须先 unconfigure。

| 字段 | 含义 |
|---|---|
| module / reason | 固定 video_decoder；结束原因 unconfigure、backend_failure 或 shutdown |
| codec / coded_width / coded_height | configure 的输入编码名和编码尺寸 |
| configured_at_unix_ms / finished_at_unix_ms | 配置成功后开始、提交前结束的系统时间 |
| configured_duration_ms | 同一窗口的 steady_clock 时长，包含暂停和等待 |
| decode_calls / drain_calls / failed_calls | 后端调用次数及其中失败次数（包括异常） |
| output_frames | 后端实际输出帧数，包括 Seek 丢弃、世代切换后过时的输出 |
| processing_total_ms / processing_max_ms | decode/drain 调用累计和最大单次墙钟耗时 |
| processing_mean_call_ms | 累计耗时 / (decode_calls + drain_calls)，无调用时为 null |
| processing_per_output_frame_ms | 累计耗时 / 输出帧数，无输出时为 null |
| output_format / output_width / output_height | 首个有效输出 buffer 的实际格式和尺寸，无输出时为 null |
| format_changed | 后续输出是否出现不同格式或尺寸；true 时元数据只描述首次输出 |

处理耗时不包含 configure、reset、unconfigure、队列等待和后续颜色转换。
这是调用的墙钟耗时，含线程调度、后端内部等待和帧封装，不等于 CPU 执行时间或 GPU 纯解码耗时。
失败调用也计入累计及平均值。每帧成本是工作量比值，不表示每一帧单独经历的延迟。
Seek 不清零；这里只统计整个配置周期，不提供 Seek 窗口耗时或宿主端到端响应。

## 线程和退出

解码器在自己的工作线程累计，不逐帧发送事件；报告只含独立值，不持有媒体资源。
同步 Notifier 回调只复制报告入队，容量为 64，不等待队列空间。
序列化、目录创建、文件写入和每条报告的 flush 都在写线程执行。
统计模块释放时取消订阅、停止接收、排空已有报告并 join。
创建失败、队列满或写入失败不改变播放结果，异常通过普通日志记录；队列丢弃数量在退出时汇总。
文件写失败后本写入器停止落盘，继续消费并统计失败报告，退出时记录失败数量。
此处 flush 不等于强制磁盘持久化；进程崩溃或断电可能丢失未完成报告。
