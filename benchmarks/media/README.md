# Benchmark media

媒体文件不提交到 Git。请把官方文件放在本地目录，并在 `manifest.csv` 中固定来源、
文件参数和 SHA-256。

## 基准素材

使用 `tools/benchmark/download-bbb.ps1` 下载并校验官方文件：

```powershell
.\tools\benchmark\download-bbb.ps1
```

当前固定文件为 Big Buck Bunny 1080p60 MP4：

- 1920×1080，H.264 High，Level 3.1，YUV420P；
- 60fps，时长 634.667 秒；
- AAC-LC，48kHz，5.1 声道；
- 文件大小 276,266,905 bytes；
- SHA-256：`91768E73427FB1E4A3D3A419CB173E7A9D97340190734C361AC56FE2BB6C8A0D`。

来源是 Blender Foundation 的[官方 Big Buck Bunny 视频](https://video.blender.org/w/dmhvQNzwBnrWy1iYzVv5g7)
及其官方 MP4 对象存储文件。影片采用 CC BY 3.0；如果之后转码或裁剪，需在结果说明
中注明修改，并保留作者、来源和许可证链接。不要把来源不明的镜像文件作为 Release 附件。
