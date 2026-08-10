#pragma once

#include <memory>

namespace semi::application {
class ApiLayer;
}

namespace semi::infra {
class Notifier;
}

namespace semi::domain {
class AudioDecoder;
class AudioFrameStore;
class AudioOutput;
class AudioPacketQueue;
class AudioResampler;
class Demuxer;
class Generation;
class VideoDecoder;
class VideoPacketQueue;
}

namespace semi::ioc {

// 模块体系装配器（见 docs/modules/ioc_container/ioc_container.md）。
//
// 进程内单例：与 SemiPlayer「全局唯一播放器」模型一致。
//   instance()  — 取壳（不触发装配）
//   assemble()  — 按 DAG 构造模块、注入依赖；bool 成功/失败（幂等成功）
//   dispose()   — 逆序释放；bool 成功/失败（幂等成功）
//
// 结果约定见 docs/error_convention.md：内部用 bool，C ABI 再映射 semi_status。
// 当前装配音频播放和视频解码链路：
// Demuxer -> AudioDecoder -> AudioResampler -> AudioOutput，
// Demuxer -> VideoPacketQueue -> VideoDecoder -> 丢弃型 VideoFrameSink；
// 工作模块注入 ApiLayer。运行期仍禁止借 IoC 做服务定位。
// 线程约定：assemble / dispose 为单线程控制面操作。
class IoCContainer {
public:
    static IoCContainer& instance();

    IoCContainer(const IoCContainer&) = delete;
    IoCContainer& operator=(const IoCContainer&) = delete;
    IoCContainer(IoCContainer&&) = delete;
    IoCContainer& operator=(IoCContainer&&) = delete;

    /// 装配当前已注册模块。已装配 → true（幂等）。失败 → false。
    [[nodiscard]] bool assemble() noexcept;

    /// 逆序释放。未装配 → true（幂等）。失败 → false。
    [[nodiscard]] bool dispose() noexcept;

    [[nodiscard]] bool is_assembled() const noexcept;

    // 仅供进程边界（api_export）在已装配后获取 ApiLayer。业务模块仍必须使用
    // 构造期注入，禁止借 IoC 做运行时服务定位。
    [[nodiscard]] std::shared_ptr<application::ApiLayer> api_layer() const noexcept;

private:
    IoCContainer() = default;
    ~IoCContainer() = default;

    bool assembled_ = false;
    std::shared_ptr<application::ApiLayer> api_layer_;
    std::shared_ptr<infra::Notifier> notifier_;
    std::shared_ptr<domain::Generation> generation_;
    std::shared_ptr<domain::AudioPacketQueue> audio_packet_queue_;
    std::shared_ptr<domain::AudioFrameStore> decoded_audio_frame_store_;
    std::shared_ptr<domain::AudioFrameStore> playback_audio_frame_store_;
    std::shared_ptr<domain::Demuxer> demuxer_;
    std::shared_ptr<domain::AudioDecoder> audio_decoder_;
    std::shared_ptr<domain::AudioResampler> audio_resampler_;
    std::shared_ptr<domain::AudioOutput> audio_output_;
    std::shared_ptr<domain::VideoPacketQueue> video_packet_queue_;
    std::shared_ptr<domain::VideoDecoder> video_decoder_;
};

} // namespace semi::ioc
