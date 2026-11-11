#include "infrastructure/ffmpeg/hardware_decode/ffmpeg_hardware_decode.hpp"
#include "infrastructure/ffmpeg/demuxer/ffmpeg_demuxer_backend.hpp"
#include "infrastructure/ffmpeg/video_decoder/ffmpeg_video_decoder_backend.hpp"
#include "infrastructure/ffmpeg/video_renderer/ffmpeg_video_renderer_backend.hpp"

#if defined(_WIN32)
#include "infrastructure/ffmpeg/hardware_decode/d3d11_hardware_decode.hpp"
#include "infrastructure/gpu/gpu_device_manager.hpp"
#include "infrastructure/gpu/d3d11/d3d11_device.hpp"
#include "infrastructure/gpu/d3d11/d3d11_video_frame_resource.hpp"
#include "infrastructure/gpu/readback/d3d11_video_readback.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#endif

#include <gtest/gtest.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <deque>
#include <filesystem>
#include <variant>

namespace semi::infra::ffmpeg::hardware_decode {
namespace {

using contracts::media::CpuVideoFrameResource;
using contracts::media::resource_as;
using contracts::video_decoder::DecodedVideoBatch;
using video_decoder::FfmpegVideoDecoderBackend;

struct FakeState {
    enum Mode { PrepareFailure, ConfigureFailure, OpenFailure, Decline, InvalidCandidate } mode = Decline;
    int prepared = 0;
    int configured = 0;
    int selected = 0;
    int wrapped = 0;
    int destroyed = 0;
};

class FakeSession final : public FfmpegHardwareDecodeSession {
public:
    explicit FakeSession(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}
    ~FakeSession() override { ++state_->destroyed; }
    std::string_view name() const noexcept override { return "fake"; }
    std::expected<void, HardwareDecodeError> configure(AVCodecContext& context) override {
        ++state_->configured;
        if (state_->mode == FakeState::ConfigureFailure) {
            // Mutate candidate state to verify that software retry reconstructs
            // all codec parameters and does not reuse this damaged context.
            context.width = 1;
            if (context.extradata_size > 0) context.extradata[0] = 0;
            return std::unexpected(HardwareDecodeError{AVERROR_EXTERNAL, "injected setup failure"});
        }
        if (state_->mode == FakeState::OpenFailure) context.codec_id = AV_CODEC_ID_HEVC;
        return {};
    }
    AVPixelFormat select_format(AVCodecContext&, const AVPixelFormat*) noexcept override {
        ++state_->selected;
        return state_->mode == FakeState::InvalidCandidate
            ? static_cast<AVPixelFormat>(INT_MAX) : AV_PIX_FMT_NONE;
    }
    std::expected<std::unique_ptr<const contracts::media::VideoFrameBuffer>, HardwareDecodeError>
    wrap_frame(AvFramePtr) override {
        ++state_->wrapped;
        return std::unexpected(HardwareDecodeError{AVERROR_EXTERNAL, "unexpected hardware frame"});
    }
private:
    std::shared_ptr<FakeState> state_;
};

class FakeProvider final : public FfmpegHardwareDecodeProvider {
public:
    explicit FakeProvider(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}
    std::expected<std::unique_ptr<FfmpegHardwareDecodeSession>, HardwareDecodeError>
    prepare(const AVCodec&) override {
        ++state_->prepared;
        if (state_->mode == FakeState::PrepareFailure) {
            return std::unexpected(HardwareDecodeError{AVERROR(ENOSYS), "injected unavailable device"});
        }
        return std::unique_ptr<FfmpegHardwareDecodeSession>(std::make_unique<FakeSession>(state_));
    }
private:
    std::shared_ptr<FakeState> state_;
};

class FfmpegHardwareDecodeTest : public ::testing::Test {
protected:
    void SetUp() override { load(SEMI_PLAYER_TEST_MEDIA_PATH); }
    void load(const std::filesystem::path& path) {
        packets.clear();
        demuxer::FfmpegDemuxerBackend demux;
        auto opened = demux.open(path.string());
        ASSERT_TRUE(opened) << opened.error().message;
        int stream_id = -1;
        for (const auto& stream : opened->streams) {
            if (const auto* video = std::get_if<contracts::media::VideoCodecConfig>(&stream.config)) {
                config = *video;
                stream_id = stream.id.value;
                break;
            }
        }
        ASSERT_GE(stream_id, 0);
        for (;;) {
            auto read = demux.read_packet();
            ASSERT_TRUE(read) << read.error().message;
            if (std::holds_alternative<contracts::demuxer::packet::BackendEndOfStream>(*read)) break;
            auto& packet = std::get<contracts::demuxer::packet::BackendPacket>(*read);
            if (packet.stream_id.value == static_cast<std::uint32_t>(stream_id)) {
                packets.push_back(std::move(packet.packet));
            }
        }
        ASSERT_FALSE(packets.empty());
    }
    void verify_software(FfmpegVideoDecoderBackend& backend) {
        std::size_t count = 0;
        auto consume = [&](const DecodedVideoBatch& frames) {
            for (const auto& frame : frames) {
                ASSERT_NE(frame.buffer, nullptr);
                EXPECT_NE(resource_as<CpuVideoFrameResource>(*frame.buffer), nullptr);
                EXPECT_EQ(frame.buffer->width(), config.coded_width);
                EXPECT_EQ(frame.buffer->height(), config.coded_height);
                EXPECT_TRUE(frame.pts_us.has_value());
                ++count;
            }
        };
        for (const auto& packet : packets) {
            auto decoded = backend.decode(packet);
            ASSERT_TRUE(decoded) << decoded.error().message;
            consume(*decoded);
        }
        auto drained = backend.drain();
        ASSERT_TRUE(drained) << drained.error().message;
        consume(*drained);
        EXPECT_EQ(count, packets.size());
    }
    contracts::media::VideoCodecConfig config;
    std::vector<contracts::demuxer::packet::EncodedPacket> packets;
};

TEST_F(FfmpegHardwareDecodeTest, UnavailableProviderFallsBackToSoftware) {
    auto state = std::make_shared<FakeState>();
    state->mode = FakeState::PrepareFailure;
    FfmpegVideoDecoderBackend backend(std::make_shared<FakeProvider>(state));
    ASSERT_TRUE(backend.configure(config));
    verify_software(backend);
    EXPECT_EQ(state->prepared, 1);
    EXPECT_EQ(state->configured, 0);
}

TEST_F(FfmpegHardwareDecodeTest, FailedSetupRetriesWithFreshSoftwareParameters) {
    auto state = std::make_shared<FakeState>();
    state->mode = FakeState::ConfigureFailure;
    FfmpegVideoDecoderBackend backend(std::make_shared<FakeProvider>(state));
    ASSERT_TRUE(backend.configure(config));
    EXPECT_EQ(state->destroyed, 1);
    verify_software(backend);
    EXPECT_EQ(state->configured, 1);
    EXPECT_EQ(state->selected, 0);
}

TEST_F(FfmpegHardwareDecodeTest, FailedCodecOpenRetriesWithFreshSoftwareContext) {
    auto state = std::make_shared<FakeState>();
    state->mode = FakeState::OpenFailure;
    FfmpegVideoDecoderBackend backend(std::make_shared<FakeProvider>(state));
    ASSERT_TRUE(backend.configure(config));
    EXPECT_EQ(state->destroyed, 1);
    verify_software(backend);
}

TEST_F(FfmpegHardwareDecodeTest, SoftwareNegotiationKeepsSessionAcrossResetAndReleasesOnUnconfigure) {
    auto state = std::make_shared<FakeState>();
    FfmpegVideoDecoderBackend backend(std::make_shared<FakeProvider>(state));
    ASSERT_TRUE(backend.configure(config));
    verify_software(backend);
    EXPECT_GT(state->selected, 0);
    EXPECT_EQ(state->wrapped, 0);
    backend.reset();
    EXPECT_EQ(state->destroyed, 0);
    verify_software(backend);
    EXPECT_EQ(state->prepared, 1);
    backend.unconfigure();
    EXPECT_EQ(state->destroyed, 1);
    backend.unconfigure();
    EXPECT_EQ(state->destroyed, 1);
    ASSERT_TRUE(backend.configure(config));
    EXPECT_EQ(state->prepared, 2);
    verify_software(backend);
}

TEST_F(FfmpegHardwareDecodeTest, InvalidCandidateFailsWithoutReplayingOrSwitchingDecoders) {
    auto state = std::make_shared<FakeState>();
    state->mode = FakeState::InvalidCandidate;
    FfmpegVideoDecoderBackend backend(std::make_shared<FakeProvider>(state));
    ASSERT_TRUE(backend.configure(config));
    bool failed = false;
    for (const auto& packet : packets) {
        auto decoded = backend.decode(packet);
        if (!decoded) { failed = true; break; }
    }
    EXPECT_TRUE(failed);
    EXPECT_EQ(state->prepared, 1);
    EXPECT_EQ(state->configured, 1);
    EXPECT_EQ(state->destroyed, 0);
    backend.unconfigure();
    EXPECT_EQ(state->destroyed, 1);
}

#if defined(_WIN32)

std::string unsupported_decode_reason(gpu::GpuDeviceManager& devices, const GUID& profile,
                                      DXGI_FORMAT format, UINT width, UINT height) {
    auto device = devices.acquire_d3d11();
    if (!device) return device.error().message;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video;
    if (FAILED((*device)->native_device()->QueryInterface(IID_PPV_ARGS(video.GetAddressOf())))) {
        return "ID3D11VideoDevice unavailable";
    }
    BOOL supported = FALSE;
    if (FAILED(video->CheckVideoDecoderFormat(&profile, format, &supported)) || !supported) {
        return "device lacks the required decoder profile/output format";
    }
    D3D11_VIDEO_DECODER_DESC description{profile, width, height, format};
    UINT configurations = 0;
    if (FAILED(video->GetVideoDecoderConfigCount(&description, &configurations)) || !configurations) {
        return "device lacks a decoder configuration for the fixture";
    }
    return {};
}

TEST_F(FfmpegHardwareDecodeTest, UnsupportedD3D11StreamNegotiatesSoftwareWithoutLosingPackets) {
    load(std::filesystem::path(SEMI_PLAYER_TEST_MEDIA_PATH).parent_path() /
         "hardware_decode/h264-high10.mp4");
    FfmpegVideoDecoderBackend backend(
        make_platform_hardware_decode_provider(std::make_shared<gpu::GpuDeviceManager>()));
    ASSERT_TRUE(backend.configure(config));
    verify_software(backend);
}

TEST_F(FfmpegHardwareDecodeTest, HevcMain10ProducesP010AndMatchesSoftwareImage) {
    load(std::filesystem::path(SEMI_PLAYER_TEST_MEDIA_PATH).parent_path() /
         "hardware_decode/hevc-main10.mp4");
    auto devices = std::make_shared<gpu::GpuDeviceManager>();
    const auto reason = unsupported_decode_reason(*devices, D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10,
                                                  DXGI_FORMAT_P010, 128, 96);
    if (!reason.empty()) GTEST_SKIP() << reason;
    auto provider = make_platform_hardware_decode_provider(devices);
    auto probe = provider->prepare(*avcodec_find_decoder_by_name(config.common.codec_name.c_str()));
    if (!probe) GTEST_SKIP() << probe.error().message;
    probe->reset();
    FfmpegVideoDecoderBackend software;
    FfmpegVideoDecoderBackend hardware(provider);
    ASSERT_TRUE(software.configure(config));
    ASSERT_TRUE(hardware.configure(config));
    auto readback = std::make_shared<gpu::D3D11VideoReadback>();
    video_renderer::FfmpegVideoRendererBackend renderer(readback);
    ASSERT_TRUE(renderer.configure({}));
    std::vector<contracts::media::RenderedVideo> cpu_images;
    std::vector<contracts::media::RenderedVideo> gpu_images;
    auto consume = [&](DecodedVideoBatch& frames, bool is_hardware) {
        for (auto& frame : frames) {
            ASSERT_NE(frame.buffer, nullptr);
            if (is_hardware) {
                ASSERT_NE(resource_as<gpu::D3D11VideoFrameResource>(*frame.buffer), nullptr)
                    << "this test requires actual HEVC Main10 hardware decode";
                EXPECT_EQ(frame.buffer->pixel_format(), contracts::media::VideoPixelFormat::P010);
            }
            auto image = renderer.render(frame);
            ASSERT_TRUE(image) << image.error().message;
            (is_hardware ? gpu_images : cpu_images).push_back(std::move(*image));
        }
    };
    for (const auto& packet : packets) {
        auto cpu = software.decode(packet);
        auto gpu = hardware.decode(packet);
        ASSERT_TRUE(cpu) << cpu.error().message;
        ASSERT_TRUE(gpu) << gpu.error().message;
        consume(*cpu, false);
        consume(*gpu, true);
    }
    auto cpu = software.drain();
    auto gpu = hardware.drain();
    ASSERT_TRUE(cpu);
    ASSERT_TRUE(gpu);
    consume(*cpu, false);
    consume(*gpu, true);
    ASSERT_EQ(gpu_images.size(), packets.size());
    ASSERT_EQ(cpu_images.size(), gpu_images.size());
    for (std::size_t index = 0; index < cpu_images.size(); ++index) {
        const auto& expected = cpu_images[index];
        const auto& actual = gpu_images[index];
        ASSERT_EQ(expected.pixels.size(), actual.pixels.size());
        EXPECT_EQ(expected.pts_us, actual.pts_us);
        std::uint64_t difference = 0;
        for (std::size_t byte = 0; byte < expected.pixels.size(); ++byte) {
            difference += std::abs(std::to_integer<int>(expected.pixels[byte]) -
                                   std::to_integer<int>(actual.pixels[byte]));
        }
        // Different native YUV layouts may round chroma conversion differently.
        EXPECT_LT(static_cast<double>(difference) / expected.pixels.size(), 3.0);
    }
}

// Actual decode, native readback and swscale; no synthetic hardware AVFrames.
TEST_F(FfmpegHardwareDecodeTest, D3D11DecodesEntireStreamAndRetainedFramesOutliveDecoder) {
    auto devices = std::make_shared<gpu::GpuDeviceManager>();
    const auto reason = unsupported_decode_reason(*devices, D3D11_DECODER_PROFILE_H264_VLD_NOFGT,
                                                  DXGI_FORMAT_NV12, 320, 192);
    if (!reason.empty()) GTEST_SKIP() << reason;
    auto provider = make_platform_hardware_decode_provider(devices);
    auto probe = provider->prepare(*avcodec_find_decoder_by_name(config.common.codec_name.c_str()));
    if (!probe) GTEST_SKIP() << probe.error().message;
    probe->reset();

    auto decoder_owner = std::make_unique<FfmpegVideoDecoderBackend>(provider);
    auto& backend = *decoder_owner;
    ASSERT_TRUE(backend.configure(config));
    video_renderer::FfmpegVideoRendererBackend renderer(std::make_shared<gpu::D3D11VideoReadback>());
    ASSERT_TRUE(renderer.configure({}));
    // Retain 4 surfaces to exercise downstream queue ownership/pool sizing.
    std::deque<contracts::media::DecodedVideo> held;
    std::size_t count = 0;
    auto consume = [&](DecodedVideoBatch& frames) {
        for (auto& frame : frames) {
            ASSERT_NE(frame.buffer, nullptr);
            ASSERT_NE(resource_as<gpu::D3D11VideoFrameResource>(*frame.buffer), nullptr)
                << "hardware test must not silently pass via software fallback";
            EXPECT_EQ(resource_as<CpuVideoFrameResource>(*frame.buffer), nullptr);
            auto rgba = renderer.render(frame);
            ASSERT_TRUE(rgba) << rgba.error().message;
            EXPECT_EQ(rgba->pts_us, frame.pts_us);
            EXPECT_EQ(rgba->width, config.coded_width);
            EXPECT_EQ(rgba->height, config.coded_height);
            EXPECT_FALSE(rgba->pixels.empty());
            held.push_back(std::move(frame));
            if (held.size() > 4) held.pop_front();
            ++count;
        }
    };
    for (const auto& packet : packets) {
        auto decoded = backend.decode(packet);
        ASSERT_TRUE(decoded) << decoded.error().message;
        consume(*decoded);
    }
    auto drained = backend.drain();
    ASSERT_TRUE(drained) << drained.error().message;
    consume(*drained);
    EXPECT_EQ(count, packets.size());
    ASSERT_FALSE(held.empty());
    auto before = renderer.render(held.back());
    ASSERT_TRUE(before);
    backend.unconfigure();
    decoder_owner.reset();
    provider.reset();
    devices.reset();
    auto after = renderer.render(held.back());
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(before->pixels, after->pixels);
    EXPECT_EQ(before->pts_us, after->pts_us);
}

TEST_F(FfmpegHardwareDecodeTest, D3D11ResetAndReconfigureDecodeAgainWhileOldSurfaceIsRetained) {
    auto devices = std::make_shared<gpu::GpuDeviceManager>();
    const auto reason = unsupported_decode_reason(*devices, D3D11_DECODER_PROFILE_H264_VLD_NOFGT,
                                                  DXGI_FORMAT_NV12, 320, 192);
    if (!reason.empty()) GTEST_SKIP() << reason;
    auto provider = make_platform_hardware_decode_provider(devices);
    auto probe = provider->prepare(*avcodec_find_decoder_by_name(config.common.codec_name.c_str()));
    if (!probe) GTEST_SKIP() << probe.error().message;
    probe->reset();
    FfmpegVideoDecoderBackend backend(provider);
    contracts::media::DecodedVideo retained;
    for (int cycle = 0; cycle < 3; ++cycle) {
        if (cycle != 1) { ASSERT_TRUE(backend.configure(config)); }
        else { backend.reset(); }
        bool output = false;
        for (const auto& packet : packets) {
            auto decoded = backend.decode(packet);
            ASSERT_TRUE(decoded) << decoded.error().message;
            if (!decoded->empty()) {
                ASSERT_NE(resource_as<gpu::D3D11VideoFrameResource>(*decoded->front().buffer), nullptr);
                if (!retained.buffer) retained = std::move(decoded->front());
                output = true;
                break;
            }
        }
        ASSERT_TRUE(output);
        if (cycle != 0) backend.unconfigure();
    }
    gpu::D3D11VideoReadback readback;
    auto cpu = readback.readback(*retained.buffer);
    ASSERT_TRUE(cpu) << cpu.error().message;
    EXPECT_NE(resource_as<CpuVideoFrameResource>(**cpu), nullptr);
}

#endif

} // namespace
} // namespace semi::infra::ffmpeg::hardware_decode
