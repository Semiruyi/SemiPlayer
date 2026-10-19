# GPU 设备管理模块设计

> 状态：第一阶段已实现，硬解和 GPU 颜色转换尚未启用。
> 位于 infrastructure/gpu，创建、复用原生 GPU 设备，不依赖 FFmpeg。
> 第一版只实现 D3D11；FFmpeg 桥接与设备层分开。domain 和 contracts 不新增 GPU 设备接口。

## Context

当前 FFmpeg 视频解码后端使用软件解码，VideoFrameBuffer 暴露 CPU plane，
转换后端通过 swscale 输出 CPU RGBA。

后续明确分阶段实现：
先接入硬解和 CPU 消费端按需回读，再把 YUV → RGBA 转换放到 GPU。
解码和 GPU 转换需要复用同一个原生设备，因此设备管理不能以 FFmpeg 上下文作为唯一资源入口。

本设计替代原先的 FFmpeg 创建设备方案。设备层持有原生 D3D11 设备，
FFmpeg 层将已有设备桥接为硬件设备上下文。
不设计跨平台通用渲染接口，不在管理器中提供解码、回读或转换能力。

## 定位与职责

| 模块 | 职责 |
|---|---|
| GpuDeviceManager | 按需创建设备、缓存成功结果、返回共享所有者 |
| D3D11Device | 持有原生 device/context、设备身份与 context 访问同步 |
| FFmpeg 设备桥接 | 将已有 D3D11 设备包装成 FFmpeg 硬件设备上下文，维护桥接引用 |
| FFmpeg 解码后端 | 按视频配置和内部策略尝试硬解、处理软解回退 |
| 后续 D3D11 转换后端 | 使用同一设备把 YUV 纹理转换为 RGBA 纹理 |
| 帧的基础设施实现 | 保持底层帧、纹理及必要设备引用有效 |
| 回读适配模块 | 按输入帧的实际资源和所属设备完成回读 |

设备管理只判断原生设备能否创建。
FFmpeg 构建是否支持 D3D11VA、当前编码/profile/位深/尺寸能否硬解，由 FFmpeg 层处理。
原生设备创建成功不等于当前视频硬解成功，也不等于解码输出纹理可以直接用于 shader 采样。

软件解码沿用现有路径，不构造一个“CPU 设备”。

## 依赖关系与目录

```text
src/infrastructure/
    gpu/
        gpu_device_manager.hpp
        gpu_device_manager.cpp
        CMakeLists.txt
        d3d11/
            d3d11_device.hpp
            d3d11_device.cpp

    ffmpeg/
        ffmpeg_raii.hpp
        hardware_device/
            ffmpeg_d3d11_device_bridge.hpp
            ffmpeg_d3d11_device_bridge.cpp

    d3d11/                     # 后续 GPU 转换实现，不在第一阶段添加
        video_renderer/
```

建议管理器命名空间：semi::infra::gpu。
第一版均为基础设施内部具体类，不建立 contracts/gpu、GpuDevice 抽象基类或通用设备句柄。

| 模块 | 依赖 |
|---|---|
| GPU 设备管理 | 当前平台的设备实现、普通日志、标准库同步 |
| D3D11 设备实现 | Windows D3D11/DXGI、原生对象 RAII |
| FFmpeg 桥接 | GPU 设备对象、libavutil |
| 后续 GPU 转换 | GPU 设备对象、D3D11 |

依赖方向是 FFmpeg 桥接 → GPU 设备层，GPU 设备层不反向依赖 FFmpeg。
GPU 设备层不依赖 domain、Generation、队列、Store、Notifier 或统计模块。
Windows 原生类型只在对应基础设施头文件中出现，不进入 domain 或公开 C API。

增加其他平台时按真实需要增加设备类型和对应实现。
当前不把 D3D11、Vulkan 等不同 API 的原生能力强行包装成一套通用函数。

## 对外接口（基础设施内部）

以下为当前主要接口，省略构造、析构和 PImpl 声明：

```cpp
struct GpuDeviceError {
    std::int32_t native_code;  // 当前 D3D11 实现的 HRESULT
    std::string message;
};

class GpuDeviceManager final {
public:
    [[nodiscard]] std::expected<
        std::shared_ptr<D3D11Device>, GpuDeviceError>
    acquire_d3d11();
};

class D3D11Device final {
public:
    using ContextGuard = std::unique_lock<std::recursive_mutex>;
    // 借用视图；持有 D3D11Device 才能保持其有效。
    [[nodiscard]] ID3D11Device* native_device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* native_context() const noexcept;

    // 同一设备的所有 context 消费者遵守这一访问约定。
    [[nodiscard]] ContextGuard lock_context();

    // 仅供原生桥接回调接入；回调存活期间必须持有设备所有者。
    [[nodiscard]] std::recursive_mutex& context_mutex() noexcept;
};
```

第一版直接用 acquire_d3d11() 表达实际支持的设备类型，
不引入仅有一个分支的通用句柄 variant，也不加入 Auto/CPU 或尚未实现的 API 枚举。
未来新增 API 后再按使用方式决定是否需要统一获取接口。

管理器不可复制或移动，通过 shared_ptr 注入；D3D11Device 也不可复制或移动。
成功请求返回共享的设备对象；设备内部通过 COM RAII 持有 device/context，
不是将原生借用指针转交给调用者释放。
native_* 仅为基础设施原生接入使用，不允许消费者释放借用引用或改变同步约定。

ContextGuard 是作用域锁，析构时解锁。
FFmpeg 回调通过 context_mutex() 接入，二者操作同一把递归锁，
不要求普通消费者手动配对 lock/unlock。

## 创建流程与缓存策略

```text
acquire_d3d11()
    → 获取管理器缓存锁
    → 已有缓存：返回同一 D3D11Device 的共享所有者
    → 没有缓存：
        创建原生 D3D11 硬件设备及 context
        完成基础初始化
        失败：返回错误，不发布半初始化对象
        成功：保存并返回设备对象
```

第一版缓存一个默认硬件设备，不增加多显卡评分、显卡切换或 WARP 软件设备兜底。
原生创建按视频使用需求设置创建参数，但不枚举每一种编码格式的支持矩阵。
当前设置 VIDEO_SUPPORT 和 BGRA_SUPPORT，尝试 feature level 11.0、10.1、10.0。
不设置 SINGLETHREADED，因为设备将跨解码和转换线程共享；不要求调试层或创建 WARP 设备。

默认适配器不意味着性能最好的显卡。失败不缓存，下次显式请求可以重试。
消费者只在配置等低频路径请求设备，不能逐帧重试创建。

## 状态与线程

没有工作线程、命令队列或通知订阅，操作在调用线程同步执行。
首次创建可能阻塞，不在实时音频回调或 Notifier 同步回调中调用。

```text
Empty --创建成功--> Ready
Empty --创建失败--> Empty
Ready --取得共享所有者--> Ready
Ready --管理器析构释放缓存--> 管理器结束
```

管理器内部 mutex 保护缓存检查和首次创建，防止并发请求重复创建设备。
第一版允许创建期间持有缓存锁；这把锁不参与逐帧操作。
持锁期间不调用消费者、不发送通知、不等待其他 worker。
管理器析构不能与成员调用并发。

### 设备 context 同步

缓存锁与 context 锁职责不同：
缓存锁属于管理器；context 递归锁属于具体 D3D11Device，随设备一起存活。

- 原生转换后端访问共享 immediate context 时，使用设备的作用域锁。
- FFmpeg 桥接在初始化硬件上下文前设置 lock/unlock/lock_ctx，
  接入同一个设备的递归锁。
- FFmpeg 内部回读同样沿用该桥接上下文的同步。
- 不允许 FFmpeg 和转换后端各维护一把互不相关的 context 锁。
- AVCodecContext 本身仍由所属解码 worker 独占；设备锁不能代替解码器状态同步。

只在必要的 context 操作范围持锁，不拿着设备锁等待队列、通知回调或其他 worker。
context 锁解决 CPU 侧共享访问，不代表 GPU 已完成执行；
后续跨阶段读取资源时仍需设计对应的 GPU 顺序、同步和资源使用约束。

## FFmpeg 设备桥接

桥接位于 infrastructure/ffmpeg/hardware_device，不进入原生设备管理模块。

```text
D3D11Device
    → av_hwdevice_ctx_alloc(D3D11VA)
    → 为 FFmpeg 设置已有原生设备及必要 COM 引用
    → 接入 D3D11Device 的递归锁回调
    → av_hwdevice_ctx_init
    → 返回由 RAII 管理的 AVBufferRef
```

桥接不会调用 av_hwdevice_ctx_create 再创建另一套设备。
FFmpeg 支持类型检查和 FFmpeg 错误码转换在桥接层完成，
不塞进原生设备错误类型。

D3D11VA 上下文释放时会释放其持有的原生 COM 接口。
桥接必须给 FFmpeg 自己持有的接口引用，不能把 D3D11Device 的借用引用直接交给它释放。
FFmpeg 自动填充的 context/video 接口与手工填写的接口，都按其所有权约定处理。

桥接还要保留 D3D11Device 的 shared_ptr，直到 FFmpeg 硬件上下文最后一个引用释放，
确保 context 锁和回调参数不会提前销毁。
可通过 AVHWDeviceContext 的 user_opaque/free 管理这份桥接状态；
初始化失败与正常销毁都必须正确释放，不能只覆盖成功路径。

AvBufferRefPtr 放在 ffmpeg_raii.hpp，使用 av_buffer_unref 释放。
解码后端将引用移交 AVCodecContext::hw_device_ctx 后，不能让原 RAII 所有者重复释放。
第一版桥接上下文按解码配置建立，不再增加全局桥接缓存；
不同桥接即便引用同一个原生设备，也共享设备的同步约定。

## 生命周期与所有权

IoC 每次装配创建一个管理器，构造不创建设备，首次 acquire_d3d11 才触发创建。
IoC 不按平台决定管理器是否存在；平台支持判断封装在设备管理模块内部。
当前只提供按需获取 D3D11 的能力，尚未实现所有 GPU API 的统一枚举和自动选择。
成功设备跨 Open/Close 复用，Seek 不清理设备缓存。
不增加播放相关的 configure/unconfigure/start/stop 接口。

```text
管理器缓存 ─────────────────────────→ D3D11Device
后续 GPU 转换后端 ─────────────────→ D3D11Device
FFmpeg 硬件上下文的桥接状态 ────────→ D3D11Device
GPU AVFrame → 帧池 → FFmpeg 硬件上下文
```

D3D11Device 持有原生 COM 资源和递归锁；FFmpeg 上下文另持有自己需要的 COM 引用。
释放管理器缓存后，其他共享所有者与合法 FFmpeg 帧仍保持相应资源有效。
只保留纹理裸指针不足以表达完整所有权；后续帧资源设计需要保留设备与必要同步状态。

设备身份对应原生设备实例，不能只比较“D3D11”类型，也不能通过 AVBufferRef 句柄地址判断。
后续 GPU 消费者应核对帧所属设备，避免误用另一个设备处理输入纹理。

正常释放遵循依赖者先释放：停止 worker，释放后端和队列帧，再释放管理器。
IoC 成员顺序、显式 dispose 以及装配失败清理都遵守该约定。

## 解码、回读和 GPU 转换的关系

```text
IoC → GpuDeviceManager
           ↓ 同一个 D3D11Device
           ├─ FFmpeg 桥接 → 硬解 → YUV 纹理
           └─ 后续 GPU 转换后端 → RGBA 纹理
```

解码后端根据视频配置与策略取得设备、建立桥接并配置硬解。
失败时由后端按策略清理硬解状态，再建立软件解码上下文。
设备或桥接创建成功均不保证当前视频硬解成功，部分初始化可能延迟到处理码流时发生。

第一条落地链路仍是：
GPU 解码帧 → domain 筛选/Store → CPU 消费端按需回读 → swscale → RGBA。
回读模块从输入帧取得实际帧池和设备上下文，无需再通过管理器重新选择设备。

后续 GPU 转换后端复用同一设备，但能否直接使用解码纹理还取决于格式、
资源绑定标志和同步等条件。具体采用 shader、video processor 或 GPU 内复制另行设计，
不因“共享设备”就承诺零拷贝。转换输出纹理由转换后端管理。
GPU RGBA 如何交付宿主，也属于后续呈现链路，不由设备管理器决定。

设备管理器不自建 buffer 池。解码 surface 交给 FFmpeg 硬件帧池管理；
帧池容量、Store 背压及 Seek 中间帧释放，在硬解链路中一起设计。

## 错误与日志

遵循[错误与结果约定](../../error_convention.md)：
设备管理同步失败使用内部 std::expected，保留原生 HRESULT 和消息。
不支持 D3D11 的平台在模块内部返回不支持错误，不调用原生 API；此时 native_code=0，
表示没有原生错误码，不代表成功，成功与否始终由 expected 表达。
FFmpeg 桥接保留 FFmpeg 错误码，在消费后端边界整理诊断，不与 HRESULT 混用。

实际创建尝试记录 API、结果、原始错误码；缓存命中不逐次记录。
管理器记录原生设备原因，桥接记录 FFmpeg 接入原因，后端记录回退决定，
避免重复输出同一份完整诊断。

第一版不主动检测设备丢失，不在运行中透明重建；缓存存在不代表设备仍健康。
缓存失效、重建与关键帧恢复需要整体设计，当前通过后端已有错误链路处理。
不承诺再次 Open 自动恢复；完整销毁并重新装配后才会重新创建设备。

## 第一阶段范围与验收

第一阶段只建立设备和桥接基础，尚不启用硬解或 GPU 转换：

1. 独立 GPU 模块创建和复用原生 D3D11 设备，提供原生资源入口和统一 context 同步。
2. FFmpeg 桥接使用该设备初始化 D3D11VA 上下文，正确管理 COM、共享所有者和回调生命周期。
3. IoC 创建管理器并注入 FFmpeg 解码后端，保留原有软件解码行为。
4. 通过独立模块验证设备/桥接能力，不为验证而在普通软件播放时强制创建设备。

测试范围：

- 重复及并发首次获取复用同一原生设备。
- 管理器释放缓存后，已取得的共享所有者仍有效。
- 桥接使用原有设备，COM 引用及初始化失败清理正确。
- 桥接保留设备所有者，使设备锁与回调参数活到最后一个上下文引用释放。
- 原生访问与 FFmpeg 回调使用同一递归锁，支持所需的递归调用。
- 设备/桥接创建失败返回明确诊断，不发布半初始化对象。
- GPU 成功场景为环境相关验证，不可用时保留原因，可跳过但不能宣称成功验证。
- 现有软件播放、IoC 退出及装配失败清理不受影响。

不为测试预先增加公共工厂接口，必要的失败注入限制在模块内部。
构建使用 MSYS2 UCRT64：GPU 模块在 Windows 下链接 d3d11，当前没有直接调用 DXGI 函数，
FFmpeg 桥接沿用 libavutil。管理器在所有平台编译和装配；只有 D3D11 实现与对应桥接按 Windows 条件编译。
非 Windows 平台请求 D3D11 时由管理器返回不支持，后端据此保留软件路径。

### 当前验证结果

MSYS2 UCRT64 Release 构建通过，完整回归 304/304 通过，其中新增设备与桥接测试 9 项，
本机 GPU 测试均实际执行、没有跳过。覆盖设备复用、并发首次获取、设备所有权、
桥接设备一致性、统一递归锁、帧池和 GPU 帧引用，以及真实 NV12 硬件帧回读。
现有 IoC 与 C ABI 播放回归通过。

已覆盖空设备参数失败；设备创建失败、内存耗尽和驱动导致的桥接初始化失败未做确定性故障注入。
非 Windows 路径尚未在本次环境中构建验证。

## 后续步骤

- 调整帧资源表达，分离 CPU plane 访问与 GPU 资源视图。
- 接入硬解配置、格式协商和软解回退。
- 在 CPU 消费端按需回读，让精准 Seek 的中间帧在回读前被筛掉。
- 接入复用同一原生设备的 GPU YUV → RGBA 转换后端。
- 实测 CPU 占用、端到端 Seek 和回读等待，不仅凭 decode 调用耗时评价收益。
- 另行设计 GPU RGBA 的宿主交付，不在本模块预留整套 shader、纹理池或跨平台呈现接口。
