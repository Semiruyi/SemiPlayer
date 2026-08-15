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
    -> raw.csv + metadata.json
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

脚本会使用 `windows-benchmark` preset，生成 `metadata.json`、原始 `raw.csv`，并
记录媒体 SHA-256、Git commit 和测试参数。

## 测试场景

### Startup

从 `open` 入队到收到第一帧 RGBA 回调，记录 `open_to_first_frame_ms`。

### Paused seek

按 `open -> play -> pause -> seek -> 新 Generation 第一帧` 执行，在媒体的 25%、50%、
75% 处测试。记录 Seek 恢复时间、首帧 PTS、目标与实际关键帧的差值，并确认 Seek
后的短暂稳定窗口内没有继续交付视频帧。

当前播放器是关键帧 Seek，因此结果不能解释为目标时间戳级精确 Seek。

### Steady playback

播放固定时间窗口，记录进程 CPU 平均值、CPU P95、峰值工作集和视频回调帧数。

## 结果解释

报告必须同时记录：

- CPU、内存、Windows 和 MSYS2 环境；
- Release 构建 preset 和 Git commit；
- 测试媒体的完整参数与 SHA-256；
- 预热次数、正式次数和测试时间；
- 中位数与 P95，而不是只报告一次最好成绩。

结果汇总：

```powershell
.\tools\benchmark\summarize-results.ps1 `
    -CsvPath .\benchmarks\results\<timestamp>\raw.csv
```

## 媒体授权

媒体来源和授权记录见 [benchmarks/media/README.md](../benchmarks/media/README.md)。
Big Buck Bunny 需要按 CC BY 3.0 保留署名、许可证链接，并注明转码或裁剪等修改。
