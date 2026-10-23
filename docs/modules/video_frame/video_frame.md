# 视频帧资源描述

当前已实现资源表达与现有 CPU 链路迁移；尚未启用硬解、按需回读或 GPU 颜色转换。
公共 C API 与宿主收到的 CPU RGBA 帧不变。

## Context

原 VideoFrameBuffer 强制提供 CPU plane，无法直接表达 D3D11 纹理。
新设计让通用帧只拥有资源、提供图像元数据及资源描述入口，消费者使用自己认识的描述进行处理。
domain 保存、筛选并传递帧，不依赖 D3D11 或 FFmpeg 原生类型。

## 接口与职责

VideoFrameBuffer 提供 pixel_format、width、height 和 resource。
resource() 返回内部稳定的 const VideoFrameResource 引用，不回读、不映射、不等待 GPU。
VideoFrameResource 是可扩展的多态描述基类，不包含平台枚举或平台专属 getter。

```cpp
const auto* cpu = resource_as<CpuVideoFrameResource>(frame);
if (!cpu) {
    // 返回不支持，或者由明确的适配层选择资源转换路径。
}
```

resource_as<T> 在统一入口通过 dynamic_cast 进行受检查查询，失败返回 nullptr。
新增资源类型不修改基类，也不要求通用契约枚举所有平台。
所有查询均返回只读描述，不取得独立的底层所有权。

## 资源类型和依赖

| 类型 | 位置 | 描述内容 |
|---|---|---|
| VideoFrameResource | contracts/media | 资源描述扩展点 |
| CpuVideoFrameResource | contracts/media | CPU plane 的只读 span；地址、大小、stride |
| D3D11VideoFrameResource | infrastructure/gpu/d3d11 | 借用的设备对象、纹理、array slice |
| FfmpegD3D11VideoFrameResource | infrastructure/ffmpeg/video_decoder | 继承 D3D11 描述，额外提供借用的 const AVFrame 指针 |

D3D11 描述属于基础设施内部共享资源契约，可以直接引用基础设施设备对象。
contracts 不反向依赖 infrastructure。D3D11 转换后端只需认识 D3D11 描述，
不必依赖 FFmpeg 帧实现；后续 FFmpeg 回读则可以查询 FFmpeg 专属描述。

CpuVideoFrameResource 的 plane_count/plane 是内存布局查询，越界 plane 返回空视图。
这些接口已从通用 VideoFrameBuffer 中移除，GPU 帧不必伪造 CPU planes。
像素格式与设备类型分开：D3D11 是存储资源类型，NV12/P010 才是图像像素格式。

## 所有权与线程

DecodedVideo 继续以 unique_ptr<const VideoFrameBuffer> 转移帧所有权，PTS 与 Generation
仍由现有 DecodedVideo/VideoFrame 表达。Store 不解释资源类型。

- 描述对象由帧持有，描述里的 span、plane 地址、设备、纹理和 AVFrame 均为借用视图。
- 使用描述时必须保持帧存活；复制描述或保存裸指针不会延长资源生命周期。
- 发布后的帧描述不修改，可随帧在 worker 间转移；不增加逐帧锁。
- CPU 像素按只读消费，GPU 操作由消费者遵守设备 context 锁和必要的 GPU 同步约定。
- CPU 侧 context 锁不表示 GPU 已完成，取得资源视图也不隐含同步或可直接采样的保证。

FfmpegVideoFrameBuffer 保留原有软件 AVFrame，内部 plane 数组支撑 CPU 描述的 span。
FfmpegD3D11VideoFrameBuffer 同时保留完整 AvFramePtr 与 shared_ptr<D3D11Device>。
完整 AVFrame 引用保持 surface/帧池有效，避免只有纹理 COM 引用而 surface 已被解码器复用。
析构先释放帧及其描述，再释放设备所有者。

## D3D11 帧构造校验

FfmpegD3D11VideoFrameBuffer::create 接收并消耗 AvFramePtr，以及设备共享所有者，
返回 expected<unique_ptr<const FfmpegD3D11VideoFrameBuffer>, FfmpegD3D11FrameError>。
失败通过 FFmpeg 风格错误码与消息返回，输入帧由 RAII 释放，不发布半有效对象。

当前验证：

- 输入为 AV_PIX_FMT_D3D11，尺寸为正，有底层 surface 引用和硬件帧池。
- 帧池为 D3D11VA，其原生设备与同步状态属于传入的 D3D11Device。
- 对外像素格式从帧池 sw_format 得到，第一版支持 NV12 与 P010LE。
- 纹理确实属于同一原生设备；array slice 非负且在纹理数组范围内。
- 纹理格式与 sw_format 一致，帧/帧池尺寸不超过实际纹理范围。
- 当前只接收单 mip、单采样的二维纹理，描述中的 array slice 不承担 mip 选择。

该工厂接收可信 FFmpeg 帧，不是任意裸地址的安全解析器。
它只包装资源，不创建新 GPU 图像、不复制像素、不执行回读。

## 当前消费链路

现有软件解码器仍只产生 CPU 帧，后端验证 CPU 描述后交付 domain。
swscale 转换后端先查询 CpuVideoFrameResource，再验证 plane 数量、跨度和大小。
GPU 或未知资源返回明确的“需要 CPU 资源”错误，不隐式回读。

```text
软件解码 → CPU 资源描述 → Store → swscale → CPU RGBA → 宿主
```

当前新增的 GPU 帧实现只通过独立 GPU 测试构造验证，不改变普通播放路径。
后续硬解链路再接入 GPU 帧输出与消费端回读，使精准 Seek 中间帧可以在回读前被筛掉。
RenderedVideo 当前仍为 CPU RGBA，GPU 转换输出及宿主交付另行设计。

## 验证范围

- CPU 软件帧的格式、plane 布局与取消配置后的资源存活。
- 类型查询支持基础/专属描述，不匹配类型返回 nullptr。
- CPU 转换拒绝未知和真实 D3D11 资源，不把 GPU 地址解释成 CPU plane。
- 真实 NV12/P010 GPU 帧的描述、array slice 与所属设备。
- GPU 帧进入 Store、弹出及随队列销毁时的所有权释放。
- 帧仍被持有时，其 surface 不被另一个帧复用。
- 拒绝空输入、软件帧、错误设备、不合法 slice/尺寸及不支持的帧池格式。

GPU 测试要求真实硬件；缺少硬件或 FFmpeg 未编译 D3D11VA 时保留跳过原因。
P010 场景以设备的 P010 纹理支持为前提。已具备硬件前提的构造失败必须视为失败。
非 Windows 不编译 D3D11 帧实现和测试；通用及 CPU 描述仍保持平台无关。

本次 MSYS2 UCRT64 Release 构建通过，完整回归 316/316 通过，新增 12 项测试。
本机 GPU 场景均实际执行，没有跳过，包含 NV12/P010 资源描述与 Store 生命周期验证。
非 Windows 构建与硬解输出接入尚未验证。
