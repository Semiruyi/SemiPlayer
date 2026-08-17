# 性能基准

本文定义 SemiPlayer 的可复现性能测量方法。性能基准是手动运行的 Release 工具，
不进入默认 `ctest`，因为 CPU、音频设备、操作系统调度和后台负载都会影响结果。

## 架构

```text
PowerShell Runner
    -> Release Benchmark Host
        -> SemiPlayer C ABI / DLL
        -> FrameObserver
        -> Windows ProcessSampler
    -> raw.csv + sync.csv + metadata.json
    -> logs/semi_player.log + benchmark.stdout.log
    -> summary.md
```

第一版通过公开 C ABI 测量宿主可观察的结果，不改变播放器的 C ABI，也不把内部
Worker 的阶段耗时伪装成端到端指标。

## 构建与运行

先下载并校验不进入 Git 的官方基准媒体：

```powershell
.\tools\benchmark\download-bbb.ps1
```

运行 Release 基准：

```powershell
.\tools\benchmark\run-release.ps1 `
    -MediaPath .\.tmp\benchmark-media\big-buck-bunny-1080p.mp4
```

脚本会使用 `windows-benchmark` preset，生成 `metadata.json`、原始 `raw.csv`、
VideoSync telemetry `sync.csv`、隔离的 `logs/semi_player.log` 和 `summary.md`，并
记录媒体 SHA-256、Git commit 和测试参数。`sync.csv` 的 session 数量会按
`(Warmups + Runs) × 场景数` 校验，避免日志缺失时生成看似完整的结果。

内存调优应至少同时比较 `peak_working_set_mib`、视频回调帧数和 CPU。单纯选择工作集
最低的容量会削弱调度抖动下的缓冲余量。

## 测试场景

### Startup

从 `open` 入队到收到第一帧 RGBA 回调，记录 `open_to_first_frame_ms`。

### Paused seek

按 `open -> play -> pause -> seek -> 新 Generation 第一帧` 执行，在媒体的 25%、50%、
75% 处测试。记录 Seek 恢复时间、首帧 PTS、目标与实际关键帧的差值，并确认 Seek
后的短暂稳定窗口内没有继续交付视频帧。

当前播放器是关键帧 Seek，因此结果不能解释为目标时间戳级精确 Seek。

### Steady playback

播放固定时间窗口，记录进程 CPU 平均值、CPU P95、峰值工作集和视频回调帧数；同时
从 VideoSync 日志记录 FPS、catch-up/stale drop、wait overshoot、wakeup late、
呈现延迟、回调耗时和 busy-wait 耗时。汇总时会排除 warmup，只统计正式 steady
playback session。

CPU 百分比按进程在采样窗口内消耗的 CPU 时间除以墙钟时间计算，不按机器逻辑处理器
数量归一化；因此 100% 约等于持续占满 1 个逻辑核，而不是整台多核机器的全部算力。

## 视频容量选择

在同一台 Windows 11 / Intel Core i7-12700 主机上，以 Big Buck Bunny 1080p60 对
视频管道容量做 3 次短时筛选。帧率是宿主 RGBA 回调数除以测量时间；峰值工作集取
3 次中位数。

| VideoFrameStore / VideoRenderedStore | 峰值工作集 | 回调帧率 |
|---:|---:|---:|
| 64 / 8（原默认值） | 347.4 MiB | 58.72 fps |
| 8 / 4 | 149.4 MiB | 58.93 fps |
| **4 / 3** | **131.8 MiB** | **58.53 fps** |
| 2 / 2 | 118.2 MiB | 58.15 fps |
| 1 / 1 | 107.2 MiB | 58.19 fps |

最终选择 4/3：相对原默认值，短测峰值工作集下降约 62%，回调帧率中位数下降约
0.3%；继续压缩到 2/2 只再节省约 14 MiB，但调度抖动下的缓冲余量和回调帧率都更低。
随后以 1 次预热、5 次正式运行、每次 30 秒复核 4/3，峰值工作集中位数为
135.4 MiB、P95 为 135.5 MiB，回调帧率为 57.5–59.1 fps。

## 结果解释

报告必须同时记录：

- CPU、内存、Windows 和 MSYS2 环境；
- Release 构建 preset 和 Git commit；
- 测试媒体的完整参数与 SHA-256；
- 预热次数、正式次数和测试时间；
- 中位数与 P95，而不是只报告一次最好成绩。
- VideoSync 的 FPS 下限、catch-up drops、wait overshoot 和 wakeup late；
- 需要区分 session 聚合指标与逐事件指标，不能仅凭 overshoot 最大值断言每次丢帧原因。

结果汇总：

```powershell
.\tools\benchmark\summarize-results.ps1 `
    -CsvPath .\benchmarks\results\<timestamp>\raw.csv
```

如果 `sync.csv` 与 `raw.csv` 位于同一结果目录，脚本会自动发现它；也可以显式传入
`-SyncCsvPath`。正式结果目录应保留 `raw.csv`、`sync.csv`、`summary.md`、
`metadata.json` 和对应日志，便于复核。

## 媒体授权

媒体来源和授权记录见 [benchmarks/media/README.md](../benchmarks/media/README.md)。
Big Buck Bunny 需要按 CC BY 3.0 保留署名、许可证链接，并注明转码或裁剪等修改。
