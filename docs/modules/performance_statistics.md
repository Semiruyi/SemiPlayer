# 性能统计

`infrastructure/statistics/PerformanceStatistics` 订阅模块统计快照，通过独立线程写 JSON Lines 文件。
由 IoC 装配，先于 Worker 创建、晚于 Worker 释放；没有新增公开 C API。
当前接入视频解码器、GPU 回读和 FFmpeg 视频渲染后端，其他模块可后续定义自身快照并增加订阅。

## 输出

默认开启，文件为当前工作目录下 `logs/performance/statistics-<Unix 纳秒>-<序号>.jsonl`。
每次 IoC 装配创建独立写入器，仅在收到报告后创建文件；普通日志会记录实际输出路径。
同一文件可包含多次 Open 对应的报告，以及不同 module 的报告。每个报告独占一行，`schema_version=1`。
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

## GPU 回读统计

GPU 模块定义 `infrastructure/gpu/readback/ReadbackStatistics`，不依赖 domain 或 FFmpeg。
IoC 向 D3D11VideoReadback 注入 Notifier；模块在顺序回读调用中本地累计，实例析构时发送一次通知。
没有发生回读时不发送报告，不会单独为回读创建文件；纯软件播放仍可生成解码统计。
独立使用时可不注入 Notifier，此时不采集时钟。

回读没有 configure/unconfigure，窗口为首次调用至实例销毁，当前通常是首次 GPU 渲染至播放器 shutdown。
Close 或 Seek 不清零，多个 Open 可能合并；不与视频解码器的配置窗口强行对齐。
不扩展 VideoReadback 契约或播放器公共 API，也不逐帧发送事件。

| 字段 | 含义 |
|---|---|
| module / backend / reason | video_readback / d3d11 / instance_destroyed |
| started_at_unix_ms / finished_at_unix_ms / observed_duration_ms | 首次回读至实例销毁的时间窗口，含暂停、等待 |
| calls / successful_calls / failed_calls | 所有回读尝试数与结果计数，包含验证失败、异常路径 |
| copied_bytes | 成功回读的有效 Y/UV 字节数，不含 staging 行留白和 SIMD 尾部 |
| input_format / input_width / input_height | 首次输入的格式和有效尺寸，包括被拒绝的首次输入 |
| input_changed | 后续输入的格式或有效尺寸是否变化，true 时元数据仅描述首次输入 |
| processing_total_ms / processing_max_ms / processing_mean_call_ms | 整个 readback 调用的累计、最大和平均墙钟耗时 |
| phases | 各阶段独立的 calls / total_ms / max_ms / mean_call_ms，没有执行时均值为 null |

阶段含义：allocate_cpu 为 CPU 输出内存创建/清零；create_staging 为 staging 缓存创建/替换；
wait_context 为设备 context 锁获取；submit_copy 为 CopySubresourceRegion 命令提交；map 为同步 Map；
copy_cpu 为映射区逐行复制到 CPU 输出；unmap 为解除映射。失败尝试也记录已执行的阶段。
计时作用域不嵌套；总耗时还包含验证、资源查询、局部清理、计时与计数开销，不要求等于阶段之和。

Map 可能同时等待先前解码、GPU 复制和驱动同步，不能称为纯 GPU 传输耗时；命令提交耗时也不是
GPU 执行时间。不插入 GPU timestamp、Flush 或额外同步点，保留原来的同步 Map 行为。
create_staging 只在实际创建/替换时计时，其 mean_call_ms 是单次创建成本；摊销成本应除以总回读次数。
不包含后续 swscale 颜色转换、CPU 输出帧在消费者处的释放、整个解码表面生命周期。

写入器使用各模块快照的 variant，共用原有有界队列与异步写线程；不会把 GPU 资源交给统计线程。
生产者必须先停止并提交，统计模块再取消所有订阅、排空队列并释放。

## 视频渲染统计

FFmpeg 渲染后端定义 `infrastructure/ffmpeg/video_renderer/VideoRendererStatistics`，
IoC 注入 Notifier；不修改 domain、媒体契约或公共 API。不注入 Notifier 时不读取统计时钟。
成功 configure 时开始新的窗口，unconfigure 时发送一次值快照，析构沿用 unconfigure；
配置失败不报告，重复取消配置不重复报告。自然 EOF 不结束窗口，Close/Shutdown 会结束。
Seek reset 清除转换器缓存，但不清零统计；窗口内出现渲染失败时继续累计，取消配置时统一提交。

| 字段 | 含义 |
|---|---|
| module / backend / reason | video_renderer / ffmpeg_swscale / unconfigure（含析构调用） |
| configured_at_unix_ms / finished_at_unix_ms / configured_duration_ms | 配置成功至取消配置的时间窗口，包含暂停和等待 |
| calls / successful_calls / failed_calls | 配置期间 render 调用数及结果，包含验证失败与异常；配置外调用不计 |
| output_bytes | 成功输出的有效 RGBA 字节数，不含 SIMD 尾部 |
| input_format / input_width / input_height | 首个通过输入和输出尺寸验证的源格式、有效尺寸，无有效输入时为 null |
| output_format / output_width / output_height | 固定 rgba8，尺寸对应上述首次输入；无有效输入时尺寸为 null |
| format_changed | 后续有效输入或输出格式尺寸是否改变，首次元数据保持不变 |
| processing_total_ms / processing_max_ms / processing_mean_call_ms | 完整 render 调用的累计、最大和平均墙钟耗时，无调用时均值为 null |
| phases | 各阶段 calls / total_ms / max_ms / mean_call_ms，无调用时均值为 null |

阶段分别为：readback（非 CPU 输入的回读调用）、create_scaler（创建/重建 swscale context）、
allocate_rgba（输出 vector 分配和清零，含 SIMD 尾部）、scale（仅 sws_scale 调用，含颜色转换及可选缩放）。
阶段彼此不嵌套，失败调用只累计实际执行的阶段。转换器缓存命中不增加 create_scaler 次数。
readback 与 video_readback 报告描述同一次操作，不能相加；两个模块的统计窗口也可能不同。
总耗时还包含参数验证、准备平面指针、临时回读帧释放、计时计数等开销，不要求等于阶段之和。
不包含队列等待、配置/reset/取消配置、SDL 上传/呈现、回调交付或消费者释放 RGBA 帧的耗时。
scale 是 CPU 调用墙钟时间，不是端到端帧延迟。评估转换成本应单看 scale，同时关注 allocate_rgba，
避免把回读 Map 等待归因于 YUV→RGBA。

### 渲染实测（2026-11-11）

MSYS2 UCRT64 Release，Big Buck Bunny H.264 1920×1080 60fps，实际输出为 D3D11VA NV12，
经原生同步回读后用 swscale 转换为同尺寸 RGBA。独立 5 秒预热后，测量 3 轮各 20 秒。
统计包含每轮初始化和 Close 前实际执行的调用，不只统计最终交付帧；不添加额外同步点。

3645 次 render 全部成功，均值 10.566 ms，最大单次 29.402 ms。
三轮均值分别为 10.504 / 10.453 / 10.742 ms；下表按总调用次数加权汇总。

| 阶段 | ms / 次渲染 | 占渲染总耗时 |
|---|---:|---:|
| 回读（含 GPU 等待） | 5.186 | 49.08% |
| swscale 转换器创建（摊销） | 0.000221 | 0.002% |
| RGBA 输出分配与清零 | 1.865 | 17.65% |
| sws_scale：NV12→RGBA，无缩放 | 3.380 | 31.99% |
| 验证、临时帧释放和其他开销 | 0.135 | 1.28% |

每轮只创建一次转换器，共 3 次，单次创建均值 0.268 ms，缓存命中正常。
benchmark 实际交付约 59 fps；render 调用数多于交付数，包含启动预取、缓存以及关闭前在途工作。
354/354 回归测试通过，新增 4 项统计验证。

当前颜色转换有优化价值：约占 render 调用墙钟耗时的三分之一，另外输出内存分配/清零也值得关注。
这不是播放吞吐受限的证据，也不能据此承诺 GPU 转换会直接节省 3.380 ms：GPU 方案仍需评估
转换执行、同步和宿主所需输出的回读成本。回读与转换目前串行发生，但解码在其他线程及 GPU
异步执行，不能把解码后端均值再相加后当作端到端延迟。

原始 JSONL、benchmark CSV、summary.json 和 phases.csv 保留于
D:/w30075696/project/tmp/renderer-statistics-20261111/。

## 线程和退出

解码器在自己的工作线程累计，不逐帧发送事件；报告只含独立值，不持有媒体资源。
同步 Notifier 回调只复制报告入队，容量为 64，不等待队列空间。
序列化、目录创建、文件写入和每条报告的 flush 都在写线程执行。
统计模块释放时取消订阅、停止接收、排空已有报告并 join。
创建失败、队列满或写入失败不改变播放结果，异常通过普通日志记录；队列丢弃数量在退出时汇总。
文件写失败后本写入器停止落盘，继续消费并统计失败报告，退出时记录失败数量。
此处 flush 不等于强制磁盘持久化；进程崩溃或断电可能丢失未完成报告。
