# 宿主视频帧回调设计

## 目标

播放器采用同步借用回调向宿主交付已经到达展示时刻的 CPU 视频帧：

- 宿主在 `open` 前配置输出像素格式、输出尺寸、帧回调和 `user_data`。
- 配置作为普通异步控制命令进入 ApiLayer 命令队列，并返回命令 handle。
- `VideoRenderer` 负责格式和尺寸转换，`VideoSync` 负责选帧并在自己的工作线程同步调用回调。
- 回调取得只读、临时借用的帧视图；回调返回后帧和像素数据立即失效。
- 播放器不会为了构造 C ABI 帧视图再次复制像素。宿主负责在回调内完成 GPU 上传，或复制到自己的内存后再异步处理。

本设计只覆盖 CPU 帧回调，不覆盖 GPU texture、共享句柄或宿主提供可写缓冲区。

项目中不新增独立的 `VideoOutput` 模块或对象。输出配置由 ApiLayer 保存并在 `open` 时拆分给 `VideoRenderer` 和 `VideoSync`；到期帧由 `VideoSync` 直接调用已配置的宿主回调。

## 数据流与职责

```text
配置：

semi_player_configure_video_output(config)
    -> ApiLayer 命令队列
    -> 保存会话前配置
    -> 返回 handle；宿主通过 await 取得最终状态

open：

ApiLayer
    -> 像素格式、输出尺寸传给 VideoRenderer::configure()
    -> 帧回调配置传给 VideoSync::configure()

帧交付：

VideoDecoder
    -> VideoRenderer（转换成宿主请求的 CPU 格式和尺寸）
    -> VideoRenderedStore
    -> VideoSync（按播放时钟选择到期帧）
    -> on_frame(user_data, borrowed_frame)
    -> 宿主同步上传 GPU，或复制到宿主自有缓冲区
    -> 回调返回
    -> 播放器销毁该 CPU 帧
```

职责边界：

- `ApiLayer`：串行执行配置命令、校验 `Idle` 状态、保存配置，并在 `open` 时编排下游配置。
- `VideoRenderer`：执行像素格式和尺寸转换，不知道宿主回调。
- `VideoSync`：决定交付时机并同步调用回调，不执行格式转换和 GPU 上传。
- C ABI 适配代码：把内部 `RenderedVideoFrame` 临时映射为 `semi_video_frame_t` 只读视图，不拥有像素存储。
- 宿主：保证回调快速返回，并在返回前结束对借用数据的访问。

## 第一版 C ABI 草案

以下签名是设计目标，落地实现时加入 `include/semi_player/semi_player.h`：

```c
/* Public header includes <stdint.h>; ABI-visible integers use fixed widths. */
typedef uint32_t semi_video_pixel_format_t;

enum {
    SEMI_VIDEO_PIXEL_FORMAT_RGBA8888 = 1
};

typedef struct semi_video_plane {
    const uint8_t *data;
    uint64_t size_bytes;
    uint32_t stride_bytes;
} semi_video_plane_t;

typedef struct semi_video_frame {
    uint32_t struct_size;

    semi_video_pixel_format_t pixel_format;
    uint32_t width;
    uint32_t height;

    uint32_t has_pts;
    int64_t pts_us;
    uint32_t generation;

    uint32_t plane_count;
    semi_video_plane_t planes[4];
} semi_video_frame_t;

typedef void (*semi_video_frame_callback)(
    void *user_data,
    const semi_video_frame_t *frame);

typedef struct semi_video_output_config {
    uint32_t struct_size;

    semi_video_pixel_format_t pixel_format;

    /* 0 表示保持媒体原始宽度或高度。 */
    uint32_t output_width;
    uint32_t output_height;

    semi_video_frame_callback on_frame;
    void *user_data;
} semi_video_output_config_t;

SEMI_API semi_handle_t semi_player_configure_video_output(
    const semi_video_output_config_t *config);
```

本接口没有 `semi_player_video_frame_release()`。宿主不取得帧所有权，也不能让借用指针越过回调边界。

`struct_size` 用于 ABI 演进。播放器只读取调用方声明大小以内、当前版本已知的字段。帧结构也携带 `struct_size`，宿主必须只读取自己已知且位于该大小以内的字段。公开整数使用 `<stdint.h>` 的固定宽度类型，像素格式使用 `uint32_t` 承载常量值，避免 C enum 底层宽度随编译器变化。

## 像素布局

第一版只支持 `SEMI_VIDEO_PIXEL_FORMAT_RGBA8888`：

- 单平面，`plane_count == 1`。
- 每个像素按内存地址递增依次为 R、G、B、A，各 8 bit。
- 第一行是画面顶部，行方向从左到右。
- `stride_bytes >= width * 4`；宿主必须按 stride 换行，不能假设永远紧密排列。
- `planes[0].size_bytes >= stride_bytes * height`。
- alpha 第一版恒为不透明 `255`。
- 未使用的 plane 清零。
- `has_pts != 0` 时 `pts_us` 才有效；时间单位为微秒。
- `generation` 与播放器内部 generation 同宽，用于标识 open/seek 后的数据世代。

RGBA 是公开交付格式，不代表解码原始格式。解码帧可以是 YUV420P、NV12、P010 等，由 `VideoRenderer` 转换后再交付。

第一版以 SDR 输出为目标，宿主把 RGBA 值作为普通 sRGB 显示数据使用。色彩原色、传递函数、矩阵和 HDR 元数据尚不进入公开 ABI，因此第一版不承诺 HDR 的色彩准确性；增加 HDR 支持前必须先扩展这些辅助字段和转换规则。

多平面结构在第一版中保留，是为了未来增加 NV12/P010 等格式时不破坏帧 ABI；在明确色彩空间、位深、plane 尺寸和对齐规则之前，不对外宣称支持这些格式。

## 配置命令语义

`semi_player_configure_video_output()` 是普通异步控制命令：

- 调用时复制可读取的配置值，或把结构校验失败记录进任务；无论哪种情况都不保留宿主传入的 `config` 指针。
- 成功入队返回非零 `semi_handle_t`；宿主通过 `semi_player_handle_await()` 取得最终 `semi_status_t`。
- `0` 表示 `config == NULL`、ApiLayer 未运行、任务容量已满或入队失败，与其他控制命令约定一致。
- 命令只允许在执行时的播放器状态为 `Idle`；否则最终状态为 `SEMI_ERR_INVALID_STATE`。
- 配置与 `open` 使用同一命令队列，因此严格按入队顺序生效。
- 配置命令先于 `open` 执行时，后续 `open` 使用新配置；`open` 先执行时，后续配置因不再处于 `Idle` 而失败。
- 配置命令支持现有的 `await` 和“尚未开始时 cancel”语义。
- `output_width == 0` 和 `output_height == 0` 分别表示保持媒体原始维度。
- 第一版只接受 RGBA8888；未知格式、非法尺寸或过小的 `struct_size` 由该命令最终返回 `SEMI_ERR_INVALID_ARGUMENT`。
- `on_frame == NULL` 表示禁用宿主视频交付；视频管道仍可工作，到期帧由 `VideoSync` 正常消费后销毁。

输出尺寸、格式、回调和 `user_data` 在一个媒体会话内保持稳定。第一版不支持播放中动态切换，避免让管道中同时存在不同布局的帧。

## 借用生命周期

调用 `on_frame(user_data, frame)` 时，播放器仍拥有 `frame` 及所有 plane 的像素存储：

- `frame` 指针、`planes` 内容和 `planes[n].data` 只在本次回调执行期间有效。
- 回调返回后，播放器立即销毁或复用相关存储；宿主不得继续读取任何借用指针。
- 宿主不得修改帧描述或只读像素数据。
- 宿主可以在回调内同步完成 GPU 上传。
- 若 GPU API 不能保证调用返回前已经脱离源 CPU 指针，宿主必须先复制到自己拥有的 staging buffer。
- 若宿主要把工作投递到 UI、渲染或其他线程，必须在回调返回前把像素复制到宿主自有内存；不能只投递播放器提供的指针。
- PTS、generation、尺寸等元数据可以按值复制后跨线程保存。

因为借用生命周期不越过回调，`seek`、`close` 和 `shutdown` 不需要管理任何已交付帧，也不存在宿主归还帧的步骤。

## 回调线程与阻塞约束

- `on_frame` 在 `VideoSync` 工作线程同步执行，不在 ApiLayer 命令线程或音频实时回调线程执行。
- 同一播放器不会并发执行两个帧回调；回调顺序就是 `VideoSync` 的交付顺序。
- 回调必须尽快返回。允许同步复制或提交 GPU 上传，但不得等待垂直同步、GPU fence、窗口事件、网络或其他可能长期阻塞的操作。
- “送显”表示提交上传或绘制工作，不表示等待画面真正扫描到屏幕。
- 回调不得抛出异常跨越 C ABI。
- 宿主不得在回调内调用 `await`、`shutdown` 或其他可能等待播放器工作线程的接口，否则可能死锁。
- `close`/`shutdown` 会等待正在执行的回调返回；它们返回后不再产生当前会话的新回调。

`user_data` 由宿主拥有，播放器只原样传回。宿主必须保证它至少存活到以下任一边界：

- 当前媒体会话的 `close` 完成；
- `shutdown` 返回；
- 没有打开媒体时，替换该配置的后续配置命令完成。

## 错误与异常边界

- 空配置指针使导出函数返回 `0`，不创建任务。
- 过小的 `struct_size`、非法格式、非法尺寸或其他配置值错误由命令最终返回 `SEMI_ERR_INVALID_ARGUMENT`。
- 命令执行时生命周期状态不允许配置，最终返回 `SEMI_ERR_INVALID_STATE`。
- 帧没有有效回调时直接销毁，不视为错误。
- C ABI 帧描述在栈上或等价的短生命周期存储中构造，不为像素数据分配新的 owner。

## 实现边界

本设计不引入 `VideoOutput`、`VideoPresentationSink`、回调适配器实例或新的 IoC 节点：

- ApiLayer 的配置命令保存内部 `VideoPresentationConfig`。
- `open` 把格式和尺寸传给 `VideoRenderer::configure()`。
- `open` 把内部帧回调传给 `VideoSync::configure()`。
- C ABI 导出边界创建一个内部回调闭包；闭包在被调用时把 `RenderedVideoFrame` 映射为临时 `semi_video_frame_t` 视图，再调用宿主函数指针。
- `VideoSync` 只调用内部回调类型，不直接依赖公开 C ABI 结构。

建议内部配置形态：

```cpp
using VideoFramePresentationCallback =
    std::function<void(const RenderedVideoFrame&)>;

struct VideoPresentationConfig {
    VideoPixelFormat pixel_format = VideoPixelFormat::Rgba8;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    VideoFramePresentationCallback on_frame;
};

struct VideoSyncOptions {
    bool audio_master = true;
    VideoFramePresentationCallback on_frame;
};
```

## 后续演进

若未来需要减少宿主侧复制或支持 GPU 零拷贝，应增加独立输出模式，而不是放宽本回调的借用生命周期：

- 宿主提供可写缓冲区：增加 `acquire/commit/cancel` 或 `render_into` 契约。
- GPU texture 或共享句柄：作为另一类输出内存类型显式协商，不能伪装成普通 CPU 指针。
- 异步持帧：若确有需求，再设计显式 retain/release 所有权协议；第一版不为此承担生命周期复杂度。

同步借用回调继续作为最小、可移植的 CPU copy-back 路径。
