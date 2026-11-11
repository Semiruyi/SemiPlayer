# FFmpeg 硬件解码适配

状态：已接入 D3D11VA，默认优先尝试硬解，准备失败或格式协商不支持时走软解。
当前输出仍为 CPU RGBA；GPU YUV → RGBA 和运行中自动切换软解不在本阶段范围内。

## 分层与依赖

```text
IoC → make_platform_hardware_decode_provider(GpuDeviceManager)
          → FfmpegHardwareDecodeProvider 抽象
              → D3D11HardwareDecodeProvider
                  → GpuDeviceManager / FFmpeg device bridge

FfmpegVideoDecoderBackend → provider → 本次配置的 session
    → 统一 send / receive / flush
    → CPU AVFrame：FfmpegVideoFrameBuffer
    → 硬件 AVFrame：session.wrap_frame
        → FfmpegD3D11VideoFrameBuffer
```

接口与实现均位于 infrastructure/ffmpeg/hardware_decode，可以使用 FFmpeg 类型。
不新增公共 contract，不让 domain 依赖设备；主解码后端不包含 D3D11 类型或平台宏。
平台条件编译集中在组装工厂和 CMake；非 Windows 工厂返回空 provider，沿用软解。
GpuDeviceManager 在所有平台正常装配，其职责仍是原生设备管理。

## Provider 与 session

- Provider 为长期注入的准备入口，依据 AVCodec 的硬解配置判断能否尝试当前实现。
- Session 是一次解码器配置的状态对象，没有 session_id；不是新的播放器会话机制。
- Session 保存设备所有者与 FFmpeg bridge，配置 AVCodecContext、参与 get_format、包装硬件帧。
- prepare/configure/wrap_frame 使用 expected，native_code 为 FFmpeg 错误码。
  原生设备失败归一为 AVERROR_EXTERNAL，消息保留原始 HRESULT，避免混用两种错误码。
- select_format 为 noexcept；返回候选格式或 NONE，NONE 表示交给后端选择受支持的软件格式。
  后端额外检查返回格式确实属于候选列表，避免不合法返回越过 FFmpeg 回调边界。

## 配置与格式协商

1. 后端验证编码配置、查找解码器。
2. Provider 检查 D3D11VA / D3D11 / HW_DEVICE_CTX 配置，按需取得原生设备并创建 bridge。
3. 后端新建解码上下文、复制编码参数和 extradata，安装统一 get_format 回调。
4. Session 附加 hw_device_ctx 引用，配置额外硬件表面数量，然后打开解码器。
5. FFmpeg 请求格式时，D3D11 session 通过 avcodec_get_hw_frames_parameters 取得实际帧池参数。
6. 只接受 NV12 或 P010LE 的池布局；初始化帧池成功后附加 hw_frames_ctx，并选择 D3D11。
7. 硬件格式不在候选列表、参数或池初始化失败、布局不支持时，后端选择受支持的软件候选。

get_format 可能延迟到处理压缩包时，也可能因分辨率变化或硬件初始化失败而重复调用。
每次均重新检查候选和帧池参数；FFmpeg 在回调前清除旧 hw_frames_ctx。
设备准备成功、avcodec_open2 成功都不能证明硬解已生效，必须看实际输出帧。

## 回退边界

| 触发条件 | 行为 |
|---|---|
| 没有 provider、解码器不支持、设备或 bridge 准备失败 | 软件配置 |
| Session 配置或硬件候选上下文 open 失败 | 销毁候选上下文与 session，用全新上下文重新复制参数并尝试软解 |
| get_format 拒绝硬件，存在受支持的软件候选 | 同次协商选择软件输出，不重放包 |
| 解码阶段出错、没有可用候选、包装硬件帧失败 | 现有 backend error 链路，不重启解码器或重放包 |

FFmpeg 可能在硬件初始化失败后再次请求格式，允许它正常协商软件候选。
这与消费压缩包后重建软件解码器是两回事：后者需要输入缓存/重放或重新定位关键帧，本阶段不实现。
设备丢失后的缓存失效与恢复同样另行设计，不承诺 Close/Open 一定能够恢复设备。

## 帧生命周期与同步

AVCodecContext::opaque 借用 session，后端先销毁解码上下文、等待 FFmpeg 工作线程退出，再释放 session。
局部配置失败和正常 unconfigure 都遵循相同顺序。
GPU 帧持有完整 AVFrame 和设备共享所有者，独立于 context/session/provider 存活。
仅保留纹理 COM 引用不足以防止表面被帧池复用，必须保留 AVFrame 引用。

解码与回读沿用原生设备同一个 context 递归锁，CPU 锁不代表 GPU 工作已完成。
回读的 Map 仍可能等待 GPU，因此后台 decode 调用耗时不等于整个 GPU 解码完成时间。

reset 调用 avcodec_flush_buffers，保留设备与 session；unconfigure 释放本次配置。
新配置会创建新的 session，但原生设备仍可由管理器复用。

## 背压与精准 Seek

当前 FrameStore 容量为 4，渲染可持有 1 帧；另留 3 个交接/批次缓冲槽，extra_hw_frames=8。
avcodec_get_hw_frames_parameters 已将 extra_hw_frames 与线程缓冲计入固定池大小，直接使用其结果，
不重复累加。若返回的是动态池，保留 initial_pool_size=0。该数量与当前下游持有量耦合；扩大队列、引入长期
持帧消费者或支持单包大量输出时，需要重新审视容量，不能作为任意持帧数量的保证。

精准 Seek 继续由 DefaultVideoDecoder 在 store_decoded_outputs 中筛掉目标 PTS 之前的帧。
被丢弃的 GPU 帧不回读、不转换；进入渲染的帧才调用现有 VideoReadback。
domain、公共 API、输出 RGBA 和原有 PTS/Generation 语义不变。

## 日志和验证

后端记录准备失败/打开失败的回退原因、硬解尝试，以及首次实际输出或格式/尺寸变化。
实际输出日志区分 software 与 session 名称；不逐帧记录，不扩展 domain 统计字段。

测试覆盖：

- Fake provider：准备失败、配置污染后重新软解、open 失败、软件协商、reset/reconfigure/session 释放。
- 无效格式候选导致错误，解码阶段不会自动重建上下文或重放包。
- 真实 H.264 D3D11 完整解码、保留 4 个表面、回读和 RGBA 转换、销毁后帧继续可用。
- HEVC Main10 实际 P010 输出，与软件解码 RGBA 图像及 PTS 比较。
- H.264 High10 软件协商，维持软件 10-bit 输出。
- IoC/ABI 播放、精准 Seek、连续 Seek、暂停、EOF、关闭重开回归。

硬件测试需对应设备及编码能力；跳过须有明确原因，不能将软件回退视为硬解验证成功。
非 Windows 编译尚未在当前环境验证。

### 本次验证（2026-11-05）

MSYS2 UCRT64 Release 构建通过，完整回归 344/344 通过，新增 9 项硬解适配测试，
本机硬件场景没有跳过。H.264 实际输出 D3D11 NV12，HEVC Main10 实际输出 D3D11 P010。

现有 benchmark 使用 Big Buck Bunny 1080p60，执行开播、25%/50%/75% 暂停关键帧 Seek、
10 秒持续播放的单次功能检查，全部完成。端到端开播首帧 99.430 ms，Seek 首帧分别为
15.319 / 13.382 / 11.873 ms，三处 Seek 均保持暂停，持续播放交付 594 帧。
这些为单次功能数据，不作为软硬解性能收益结论；精准 Seek 的性能未在这次 benchmark 中测量。

构建与回归日志：build-first-frame/hwdecode-build.log、hwdecode-regression.log。
Benchmark CSV：D:/w30075696/project/tmp/hwdecode-run/smoke-final.csv。
