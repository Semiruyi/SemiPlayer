# 视频帧回读

消费端按需取得 CPU 像素的同步能力。契约位于 contracts/video_readback，原生实现位于
infrastructure/gpu/readback，不依赖 domain 或 FFmpeg。当前支持 D3D11 NV12/P010。
转换后端已接入回读，正常播放可接收 [D3D11 硬解](../hardware_decode/hardware_decode.md) 输出；GPU 颜色转换尚未启用。

## 职责与依赖

```text
DefaultVideoRenderer（不变）
    → VideoRendererBackend 契约
        → FfmpegVideoRendererBackend
            ├─ CPU 资源 → swscale
            └─ 非 CPU 资源 → VideoReadback 契约
                                → D3D11VideoReadback
                                → 独立 CPU 帧 → swscale
```

| 组件 | 职责 |
|---|---|
| contracts/video_readback/VideoReadback | 规定输入借用、CPU 结果所有权和失败表达 |
| infrastructure/gpu/readback/D3D11VideoReadback | 原生 staging、子资源复制、映射、像素回读 |
| infrastructure/gpu/readback/detail/OwnedCpuVideoFrameBuffer | 回读内部的 CPU 像素内存和 plane 描述 |
| FfmpegVideoRendererBackend | 判断是否需要回读，验证 CPU 结果并做 swscale 转换 |
| IoC | 创建回读实现，通过契约注入转换后端 |

原生回读只查询 D3D11VideoFrameResource，使用纹理、array slice 和所属设备。
即使帧由 FFmpeg 产生，也无需知道 AVFrame 或 FFmpeg 帧池；输入帧自身负责保活这些资源。
回读不依赖 GpuDeviceManager，不重新选设备，不访问播放器会话状态。

## 契约

```cpp
std::expected<std::unique_ptr<const media::VideoFrameBuffer>, ReadbackError>
readback(const media::VideoFrameBuffer& input);
```

- 同步、按顺序使用；同一实例不能并发调用。
- 调用期间由调用者保持输入帧存活；回读不修改输入。
- 成功结果独立拥有 CPU 内存，提供 CpuVideoFrameResource，保留有效宽高和像素格式。
- 不缩放、不转换颜色、不携带或处理 PTS/Generation。
- 不认识的资源、非法输入或后端失败返回 std::unexpected。
- 资源描述依然是借用视图，不因为取出描述就延长资源生命周期。

ReadbackError 包含操作阶段 Validate/Allocate/CreateStaging/Map、后端原始错误码和消息。
D3D11 原生错误码为 HRESULT；没有原生错误码时填 0，失败仍由 expected 表达。

## 原生 D3D11 流程

1. 验证资源、原生设备一致性、像素格式、有效尺寸及 array slice。
2. 为独立 CPU 结果分配内存。
3. 根据原生设备身份、纹理分配尺寸和格式，创建或复用一个 staging texture。
4. 获取输入设备的 context 递归锁。
5. 使用 CopySubresourceRegion 复制指定 slice，源/目标均为单 mip、单采样。
6. Map 等待 staging 可被 CPU 读取，按 RowPitch 逐行复制有效像素。
7. 通过作用域清理 Unmap，再释放 context 锁。

第一版支持 NV12 和 P010 的单 mip、单采样纹理；不支持的格式明确失败。
纹理分配宽高必须满足 planar 格式要求，但有效图像宽高可以是奇数。
UV 起始地址按 RowPitch × 纹理分配高度计算，不能使用有效图像高度。
UV 的有效行宽按色度采样向上取整，P010 每个分量占两个字节。

Map 后任意返回路径都必须 Unmap。第一版锁覆盖复制、Map、CPU 拷贝和 Unmap，
保持同步简单，不引入异步完成状态机、额外线程或回读队列。

## CPU 所有者与缓存

OwnedCpuVideoFrameBuffer 位于回读模块的 detail 目录和 gpu::detail 命名空间，
仅作为回读内部实现，不建立独立 media 模块，不把具体内存分配方式放进 contracts。
消费者只接触 VideoReadback 契约返回的 VideoFrameBuffer，不依赖这个具体类型。
它持有像素 vector、两个 plane 描述及 CpuVideoFrameResource；发布后通过 const 帧接口消费。
构建/填充时可使用 writable_plane，不能在发布后修改或调整底层内存。
对象不可复制或移动，转移 unique_ptr 不改变描述和像素地址。
保留未暴露的 SIMD 尾部留白，满足现有 CPU 转换消费者的访问要求。

回读实例只缓存一个 staging texture，不缓存输出 CPU 副本。
同设备、同格式和同分配尺寸复用；条件变化时创建新 staging，成功后替换旧缓存。
缓存持有原生 COM 引用，保证原生设备身份有效，不保存输入帧的借用描述或设备封装裸指针。
每次 context 同步均使用当前输入帧关联的设备，不能使用上次输入的锁。

缓存随回读实例析构释放；Seek/reset、Close/unconfigure 不强制销毁可复用 staging。
IoC 所有者随转换后端释放；构造回读对象不会创建设备，不影响纯软件路径。

## 转换后端接入

FfmpegVideoRendererBackend 构造期注入 shared_ptr<VideoReadback>，不依赖原生 D3D11 实现。
CPU 输入直接进入既有校验和 swscale；非 CPU 输入才调用回读。
未注入回读时继续明确拒绝非 CPU 输入，便于独立使用纯 CPU 后端。

回读结果必须非空、提供 CPU 资源，并与输入的尺寸和格式一致；随后继续验证 plane 布局。
结果在 swscale 返回前保持存活。最终 RGBA 仍取原输入的 PTS，不从回读结果推测时间。
回读失败整理成现有 Render 错误，保留原生错误码和诊断，走已有后端错误链路。
不在消费端做软解回退，也不把回读结果重新塞入 domain Store。

“按需”指需要 CPU 转换才回读：精准 Seek 中被解码侧筛掉的中间帧不会回读，
进入转换阶段后又被同步模块丢弃的帧仍可能已经回读；不承诺只为最终显示帧回读。

## 平台与验证

D3D11VideoReadback 的原生实现封装在 Windows 分支内部，其他平台请求时返回不支持。
通用契约、CPU 所有者和转换后端不依赖 Windows 类型；本次未验证非 Windows 构建。

MSYS2 UCRT64 Release 构建通过，完整回归 335/335；新增 19 项测试，本机无 GPU 测试跳过。

- 原生夹具直接创建 NV12/P010 纹理，不借助 AVFrame 或 FFmpeg 回读。
- 按行比较已知像素，覆盖不同 array slice、奇数有效尺寸、纹理留白和 CPU stride。
- 连续读取、尺寸/格式/设备变化、旧 CPU 结果保活和源设备封装释放。
- 原生纹理通过注入回读和 swscale 得到 RGBA。
- fake 回读验证 CPU 绕过、PTS、错误传播以及空/非 CPU/元数据改变/截断结果的拒绝。
- 现有 IoC 与 C ABI 软件播放回归通过，domain 源码无改动。

未注入真实设备丢失、内存耗尽或驱动 Map 失败；错误传播已通过 fake 实现验证。
缓存复用由实现控制，当前验证连续输入与条件切换结果，不对原生创建调用次数增加测试专属接口。

## 性能统计

构造时可注入 Notifier，本地累计整次调用及 allocate_cpu、create_staging、wait_context、submit_copy、
map、copy_cpu、unmap 七个阶段的调用次数、累计和最大墙钟耗时。
在回读实例析构时发送一次 `ReadbackStatistics`，由现有性能统计模块异步写入同一 JSONL 文件，
module 为 video_readback。未调用的实例不发送，不注入 Notifier 时不采集时钟。

统计定义位于本模块，不引入 domain/FFmpeg 依赖；公共契约和渲染调用方式不变。
窗口覆盖首次调用至实例销毁，Seek/Close 不清零；当前 IoC 在播放器 shutdown 时释放回读实例。
格式/尺寸变化记录 input_changed，首帧元数据不被覆盖；不持有输入帧或原生资源。
Map 时间包含 GPU 完成等待，无法据此单独判断 PCIe 传输或解码成本；后续 swscale 不在回读统计范围内。
完整字段和计时口径见[性能统计](../performance_statistics.md)。

### 实测（2026-11-11）

MSYS2 UCRT64 Release 构建，Big Buck Bunny H.264 1080p60，D3D11VA NV12 → 原生回读 → CPU RGBA。
先播放 5 秒预热，再运行 3 轮各 20 秒。以下为模块自身 JSONL 的汇总，包含每轮初始化及关闭时
实际执行的回读，不只统计最终呈现帧；没有额外插入 GPU 同步。

3633 次回读全部成功，平均总耗时 5.058 ms，最大单次 26.106 ms；三轮均值分别为
5.114 / 4.983 / 5.078 ms。各阶段按全部回读次数摊销：

| 阶段 | ms / 次回读 | 占总耗时 |
|---|---:|---:|
| CPU 输出内存分配与清零 | 0.830 | 16.40% |
| staging 创建/替换 | 0.000128 | 0.003% |
| 设备锁获取 | 0.001952 | 0.039% |
| 复制命令提交 | 0.004374 | 0.086% |
| Map | 3.965 | 78.38% |
| CPU 像素拷贝 | 0.249 | 4.92% |
| Unmap | 0.001735 | 0.034% |
| 验证、计数与其他清理 | 0.006566 | 0.13% |

每轮只创建一次 staging，共 3 次，平均单次创建 0.155 ms，缓存复用正常。
同轮解码后端调用摊销约 0.354 ms/输出帧；Map 承担先前异步 GPU 工作的等待，不能只用该后端
调用时间代表解码完成，也不能把 Map 的 3.965 ms 全部当作显存到内存的纯传输时间。
后续 swscale 未包含在这些指标中。

完整回归 350/350 通过，新增 6 项统计验证。原始数据和汇总保留于
D:/w30075696/project/tmp/readback-statistics-20261111/steady/logs/performance/ 与 summary.json、phases.csv。
