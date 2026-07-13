# GpuDevice 模块设计

> 基础设施模块。**GPU 设备契约**——抽象"一个 GPU 设备"的共性能力(设备 + 内存),屏蔽不同图形 API 的差异。不依赖 FFmpeg、不感知任何播放器业务语义。
> 跨平台在 IoC 装配期解决:按平台选具体实现(D3D11GpuDevice / VulkanGpuDevice / ...)。消费者拿到的是 `std::shared_ptr<GpuDevice>` 契约,不知背后是什么 API。

## Context

硬解需要 GPU 设备(NVDEC/QSV 等专用解码芯片挂在图形设备上),且硬解产物(GPU 原生帧)是绑设备生命周期的 GPU 资源。若各工作模块各自创建图形设备,会有:重复创建开销、跨设备传纹理抵消硬解收益、设备生命周期难管。

GpuDevice 把"一个共享 GPU 设备"抽出来,作为基础设施注入给需要它的模块。它**只提供设备能力,不做任何播放器业务**(不硬解、不转换、不合成)。

---

## 定位

```
IoC 装配(按平台 cfg):
  Windows → D3D11GpuDevice        ┐
  Linux   → VulkanGpuDevice       ├── 都实现 GpuDevice 契约
  ...                             ┘
                    │
                    ▼ std::shared_ptr<GpuDevice> 注入
            FfmpegVideoDecoder(消费者, copy-back 路下唯一)
```

- **基础设施层,第 0 层**(无依赖,被工作模块依赖)。与 Notifier/Generation 同级。
- **纯契约,不依赖 FFmpeg**:不知道 `AVHWDeviceContext`、不知道"硬解"、不知道"视频帧"。
- **跨平台在装配期解决**:具体实现按平台 cfg 注册到 IoC,运行时消费者只面对契约。
- **不做**:不硬解(decoder 的事)、不格式转换(VideoRenderer CPU 的事)、不合成(Compositor CPU 的事)、不交付纹理(VideoSync 的事)。

---

## 职责

抽象"一个 GPU 设备"的共性,屏蔽 D3D11/Vulkan/OpenGL 等 API 差异:

1. **提供 GPU 设备**:让消费者能拿到底层设备句柄(用于构造第三方桥接,如 FFmpeg hwcontext)。
2. **提供 GPU 内存**:分配/复用 GPU buffer(装硬解产物等),绑设备生命周期管理。
3. **告知设备 API 类型**:让消费者知道该用什么方式桥接(D3D11 句柄→D3D11VA hwcontext,Vulkan 句柄→Vulkan hwcontext)。

**不包含**:
- ❌ FFmpeg 任何类型(不依赖 FFmpeg)。
- ❌ 硬解/视频帧语义(不知业务)。
- ❌ shader/pipeline/texture 跨上下文共享(直通路能力,本阶段不实现,抽象基类不留这些方法)。

---

## 契约接口

```cpp
class GpuDevice {
    // 设备的图形 API 类型(让消费者知道怎么桥接)
    virtual GpuApi api_type() = 0;

    // 该 API 的原始设备句柄(用于构造第三方桥接,如 FFmpeg hwcontext)
    virtual DeviceHandle device_handle() = 0;

    // 分配一块 GPU 内存,用完 RAII 归还内部池
    virtual GpuBufferGuard acquire_buffer() = 0;
    virtual ~GpuDevice() = default;
};

enum class GpuApi {
    D3D11,
    Vulkan,
    OpenGL,
    VaApi,   // TODO
}

struct DeviceHandle {
    // D3D11(ID3D11Device*) / Vulkan(VkDevice) / OpenGL(GLContext)
    // actual: std::variant or platform branch holding raw handle
    // ...
}
```

### 三个接口的职责

| 接口 | 返回 | 职责 |
|------|------|------|
| `api_type()` | `GpuApi` | 告诉消费者"我是什么图形 API 的设备",据此选桥接方式 |
| `device_handle()` | `DeviceHandle`(按 api_type 的枚举) | 提供该 API 的原始设备句柄,供构造第三方桥接 |
| `acquire_buffer()` | `GpuBufferGuard`(RAII) | 分配一块 GPU 内存,析构时自动归还内部池 |

### GpuApi / DeviceHandle 枚举:契约里唯一的多 API 知识

这两个枚举是契约必不可少的"多 API 知识"——消费者必须知道是什么 API 才能桥接 FFmpeg。它们是契约的一部分,不算 GpuDevice 染业务/染第三方。枚举里 Vulkan/OpenGL/VaApi 标 TODO,MVP 只实现 D3D11 分支。

### device_handle 的受控暴露

`device_handle()` 必然要暴露底层句柄给消费者(否则没法构造 FFmpeg hwcontext)。用 `DeviceHandle` 枚举包着是"受控暴露"——比直接吐 `*mut ID3D11Device` 有结构。约束靠**文档约定**:`device_handle()` **只用于构造第三方桥接(FFmpeg hwcontext),不得用于业务渲染操作**。GpuDevice 的封装边界靠约定守住(类型系统拦不住,但消费者只有 FfmpegVideoDecoder,可控)。

---

## 具体实现(IoC 装配期选,都实现抽象基类)

| 实现 | 平台 | api_type | 说明 |
|------|------|----------|------|
| **D3D11GpuDevice** | Windows(MVP) | D3D11 | 持有 ID3D11Device;buffer 池用 D3D11 texture 槽位 |
| VulkanGpuDevice | TODO(跨平台) | Vulkan | 持有 VkDevice |
| OpenGlGpuDevice | TODO | OpenGL | 持有 GLContext |
| VaApiGpuDevice | TODO(Linux 硬解) | VaApi | Linux 硬解常用 |

**MVP 只实现 D3D11GpuDevice**,但抽象基类 + 枚举已含其他 API 位。未来加后端只加派生类,不改抽象基类/消费者核心逻辑。

---

## 消费者如何用(以 FfmpegVideoDecoder 为例)

GpuDevice 不被 ApiLoop 直接调(它是资源不是工作模块)。VideoDecoder(实际是 FfmpegVideoDecoder 派生类)configure 时从注入的 `std::shared_ptr<GpuDevice>` 取设备能力:

```cpp
FfmpegVideoDecoder::configure(config, std::shared_ptr<GpuDevice> gpu) {
    // 1. 识别 API 类型
    switch (gpu->api_type()) {
        case D3D11:  用 gpu->device_handle() 的 D3D11 句柄构造 AVHWDeviceContext<D3D11VA>; break;
        case Vulkan: 用 Vulkan 句柄构造 AVHWDeviceContext<Vulkan>; break;
        // ...    各 API 的 FFmpeg 桥接分支
    }
    // 2. 配置 FFmpeg 解码器(挂 hwcontext;硬解输出格式由 decoder 自选,如 NV12)
    codec_ctx.hw_device_ctx = hw_ctx
    // 3. 解码循环里用 GpuDevice buffer 池装硬解帧
    loop:
        buf = gpu->acquire_buffer()          // 取 GPU buffer 槽位
        av_hwframe_get_buffer(...)          // 硬解到这个 buffer(GPU)
        ... download 到 CPU ...             // copy-back: av_hwframe_transfer_data
        // buf 析构即 RAII 归还池
        喂 VideoFrameStore(CPU 帧)
```

GpuDevice 全程**不知道 FFmpeg、不知道硬解、不知道视频帧**,只提供设备句柄和 buffer 池。FFmpeg 桥接(构造 hwcontext)、硬解输出格式选择、download 都归 FfmpegVideoDecoder。

### VideoDecoder 的抽象基类 + 具体派生类(通用逻辑可换后端)

```
class VideoDecoder {                      // 通用逻辑契约 (抽象基类)
public:
    virtual void configure(config, std::shared_ptr<GpuDevice> gpu) = 0;
    virtual void start() = 0; virtual void stop() = 0; virtual void seek(int64_t pos) = 0;
    virtual void decode_loop() = 0;            // 取包→解码→喂 Store 的通用骨架
};

struct FfmpegVideoDecoder {             // 具体实现,依赖 FFmpeg + GpuDevice
    gpu: std::shared_ptr<GpuDevice>,
    ...
}
```

通用骨架(decode_loop、世代号检查、喂 Store)在 base class/shared code里,FFmpeg 桥接是 FfmpegVideoDecoder 内部的 switch 段。换解码后端(如 AVFoundation/VideoToolbox)→ 写新派生类,通用骨架复用。

---

## 依赖与层级

- **层级**:基础设施层,第 0 层(无依赖,被工作模块依赖)。与 Notifier/Generation/IoCContainer 同级。
- **构造期注入给**:copy-back 路下仅 `FfmpegVideoDecoder`(通过 VideoDecoder 抽象基类注入 `std::shared_ptr<GpuDevice>`)。
- **自身依赖**:无(不依赖任何播放器模块,不依赖 FFmpeg)。
- **平台后端**:IoC 装配期按 #ifdef 选(MVP: D3D11GpuDevice)。

---

## 线程

**无线程**(基础设施,被工作模块在自己线程调用)。
- FfmpegVideoDecoder 在它的 loop 线程调 `api_type`/`device_handle`/`acquire_buffer`。
- buffer 池内部需线程安全(目前单消费者,仍用 Mutex/无锁池,为未来多消费者预留)。

---

## 关键设计决策

### 为什么硬解只用于解码(不用于格式转换/合成)
**硬件加速用在吞吐瓶颈,且成果能留在硬件消费的场景**:
- **解码是吞吐瓶颈**:软解 1080p 吃满一个 CPU 核、4K/HEVC 扛不住;硬解(NVDEC 专用芯片)降到几 % CPU,数量级差异。值得用硬件。
- **格式转换不是瓶颈**:sws_scale 转 NV12→RGBA 约 1-3ms/帧(CPU),30fps 占不到单核 10%。
- **合成更不是瓶颈**:逐像素 alpha 叠加 <1ms/帧;字幕几秒才变一次,大多数帧原样复制。
- **致命点**:copy-back 路线下,转换/合成的成果必须回 CPU(下游在 CPU + 交付 Flutter 要 CPU buffer)。GPU 转完再 download = 两次 GPU↔CPU 拷贝 + 同步,可能比 CPU 直接做还慢。**硬件加速用完还要回 CPU,那这段硬件就白做了**。

> 要点:硬件加速不是越多越好,用在瓶颈且成果能留在硬件的场景。格式转换/合成虽能 GPU 化,但成果必须回 CPU,拷贝/同步开销抵消算力收益,得不偿失。

### GpuDevice 是契约,不是某个具体 API 的封装
GpuDevice 抽象"GPU 设备共性"(设备 + 内存),不讲任何具体 API。具体设备(D3D11/Vulkan/OpenGL)是 IoC 装配期选的实现。这是**依赖倒置**——消费者依赖抽象契约,IoC 决定具体实现。换平台只改 IoC 装配,消费者零改动。

### 为什么 FFmpeg 桥接归 decoder,不归 GpuDevice
`AVHWDeviceContext` 是 FFmpeg 把平台 device 封装成自己能用的形式——这是 **FFmpeg 的桥接逻辑**,该归懂 FFmpeg 的 FfmpegVideoDecoder,不该归 GpuDevice。若 GpuDevice 构造 hwcontext,等于让它替 decoder 干 FFmpeg 的活,且染上 FFmpeg 依赖(违反基础设施层纯净)。GpuDevice 只提供设备句柄,decoder 据此自构 hwcontext——GpuDevice 不依赖 FFmpeg,职责干净。

### 为什么 device_handle 暴露原始句柄可接受
契约必然要暴露底层句柄给 decoder(否则没法构造 hwcontext)。用 `DeviceHandle` 枚举包着是"受控暴露",比直接吐裸指针有结构。约束靠文档约定:只用于构造第三方桥接,不得用于业务渲染。消费者只有 FfmpegVideoDecoder,可控。这是把 FFmpeg 桥接移到 decoder 的必然代价,换来 GpuDevice 纯净 + decoder 可换后端,值得。

### 为什么 MVP 只 D3D11 后端
跨平台 GPU 抽象的复杂度大多在"胶水"(D3D11/Vulkan/Metal 各一套 + 跨平台纹理共享),对"音视频理解"目标 ROI 低。D3D11 单后端能跑硬解 + 出画面,够展示能力。多后端留抽象基类/枚举扩展点,面试时讲"MVP 选单后端 D3D11,抽象基类留多 API 扩展,因为跨平台 GPU 胶水 ROI 低"——体现取舍判断。

### 为什么 copy-back 而非 GPU 直通
copy-back:硬解在 GPU(download 是单次拷贝),格式转换/合成在 CPU。已拿到硬解主要收益(解码卸载到专用芯片),且全链路 CPU 可跑、可单测、跨平台简单。GPU 直通的合成/共享复杂度在跨平台胶水,ROI 低。详见"为什么硬解只用于解码"。

### download 收进 VideoDecoder 内部
VideoDecoder 解完硬解帧立刻 download,喂 VideoFrameStore 的是 CPU 帧。这样:① VideoFrameStore 存 CPU 帧,和其它 Store 模式统一;② GPU 帧用完即还池,持有时间最短;③ VideoRenderer 纯 CPU 不依赖 GpuDevice,GpuDevice 只服务 decoder;④ VideoRenderer 可独立单测。

---

## 阶段二:GPU 直通升级路径(本阶段不实现)

> **为什么分两阶段**:MVP(阶段一)走 copy-back 快速验证整条管道能跑、能出画面,把可跑成果兜底;阶段二再升级零拷贝 GPU 直通,消除 download 的 GPU→CPU 拷贝。分阶段的理由:① Flutter CPU buffer 接入最通用、跨平台最简单,先保证能跑;② download 开销可接受(1080p 1.3-4ms/帧,硬解收益仍保留一个数量级);③ 直通的复杂度集中在中后段(纹理共享/shader),前置依赖(硬解/AudioClock)和 copy-back 共用,先跑通再升级风险低;④ 做完 copy-back 对"为什么 download"有真实体感,再做直通时设计决策更扎实。

### 直通的完整数据流(阶段二目标)

```
文件→[Demuxer]→VideoPacketQueue→[VideoDecoder 硬解 GPU]→GPU原生帧
                                                  ↓ (不 download,留在 GPU)
                                  [VideoRenderer GPU shader:NV12→RGBA]→GPU RGBA texture
                                                                      ↓
            [Compositor GPU合成]←SubtitleFrameStore(GPU上传的字幕位图)
                    ↓
            FinalFrameStore(GPU texture)→[VideoSync]→Flutter(纹理共享,零拷贝)
```

对比 copy-back:消除 3 次 GPU↔CPU 往返(硬解帧 download、字幕上传、最终帧 upload 给 Flutter),全程 GPU 零拷贝。

### GpuDevice 阶段二新增能力(抽象基类加方法)

| 新增接口 | 用途 | 消费者 |
|---------|------|--------|
| `create_shader_pipeline(desc)` | 建 GPU shader(顶点/片元/采样器/render target) | VideoRenderer(NV12→RGBA)、Compositor(叠加) |
| `alloc_texture(fmt, w, h)` | 分配目标 GPU texture(装转换/合成结果) | VideoRenderer、Compositor |
| `shared_handle_for_flutter(tex)` | 跨上下文纹理共享(D3D11 shared handle / Vulkan external memory),给 Flutter ExternalTexture 零拷贝 | 平台纹理交付层 |
| `upload_to_texture(cpu_buf)` | CPU buffer→GPU texture 上传(字幕位图上 GPU) | SubtitleRenderer |

各后端派生类(D3D11GpuDevice/VulkanGpuDevice/...)新增对应能力。抽象基类扩充,消费者按需调用。

### 各模块的改动(模块结构不变,只切实现)

| 模块 | copy-back(阶段一) | GPU 直通(阶段二) |
|------|-------------------|-------------------|
| VideoDecoder | 硬解 + download,喂 CPU 帧 | 硬解,**不 download**,喂 GPU 帧句柄 |
| VideoFrameStore | CPU 原生格式帧(NV12) | GPU texture 句柄 |
| VideoRenderer | CPU sws_scale | GPU shader 转 + alloc_texture 目标 |
| VideoRenderedStore | CPU RGBA buffer | GPU RGBA texture 句柄 |
| SubtitleRenderer | CPU libass 位图 | CPU libass + upload_to_texture 上 GPU |
| SubtitleFrameStore | CPU RGBA 位图 | GPU texture 句柄 |
| Compositor | CPU 像素叠加 | GPU shader 合成 + alloc_texture 目标 |
| FinalFrameStore | CPU RGBA buffer | GPU texture 句柄 |
| 平台纹理交付 | CPU buffer→Flutter texture 上传 | shared_handle_for_flutter 零拷贝 |

**模块结构不动**:Store 的名义、模块依赖关系、ApiLoop 命令编排、世代号机制全不变。只是 GpuDevice 加能力 + 6 个模块切实现。这是契约抽象预留的价值——升级时不动地基。

### 为什么 MVP 没做直通(可讲的取舍)

1. **Flutter CPU buffer 接入最通用**:跨平台最简单,先保证能跑。GPU texture 共享(Android SurfaceTexture/iOS MetalKit/桌面 GL texture)各平台机制不同,胶水多。
2. **download 开销可接受**:1080p 0.3-1ms/帧 + sws 1-3ms = 1.3-4ms/帧,远小于软解 20-40ms/帧,硬解收益仍保留一个数量级。
3. **直通的收益是消除这次拷贝**:download 开销虽小,但全程零拷贝是"正确"的终态。阶段二做直通消除它,体现从"够用"到"最优"的演进。
4. **业界标杆已验证可行**:media_kit 已在 Flutter 实现 GPU 直通,证明路径可行、收益真实,不是空中楼阁。



---

## 坑与边界

### device_handle 的使用约束
`device_handle()` 暴露原始句柄,消费者可能滥用(直接做渲染操作)。靠文档约定守住:只用于构造第三方桥接。类型系统拦不住,但消费者单一(FfmpegVideoDecoder),review 可控。

### buffer 池的容量与背压
GPU buffer 数量有限(D3D11 texture 占显存)。池容量要和 VideoFrameStore 水位配合——decoder 解太快、Renderer 消费慢时,池可能耗尽。acquire_buffer 在池空时是阻塞还是返回 None(让 decoder wait),归实现阶段定。

### 平台设备丢失(D3D11 device removed)
GPU 设备可能因驱动/硬件问题丢失(device removed)。GpuDevice 需处理重建或上报错误。本阶段标 TODO,先假设设备稳定。

### FFmpeg hwcontext 的所有权
decoder 用 device_handle 构造的 `AVHWDeviceContext`,所有权归 decoder 还是 GpuDevice?倾向归 decoder(decoder configure 时建,unconfigure 时释放),GpuDevice 只提供句柄不管 hwcontext 生命周期。

---

## 边界（本文档不涉及）

- ❌ D3D11 device 的具体创建参数 / buffer 池的内部实现 → 实现阶段
- ❌ FFmpeg hwcontext 构造的具体调用 → FfmpegVideoDecoder 文档 / 实现阶段
- ❌ GPU 直通的 shader/pipeline/跨上下文共享 → 未来阶段
- ❌ 其他平台后端(Vulkan/OpenGL/VaApi)的实现 → 未来阶段
- ❌ 平台设备丢失处理 → 未来阶段
