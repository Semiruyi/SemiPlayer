#include "infrastructure/gpu/readback/d3d11_video_readback.hpp"
#include "infrastructure/gpu/d3d11/d3d11_device.hpp"
#include "infrastructure/gpu/d3d11/d3d11_video_frame_resource.hpp"
#include "infrastructure/gpu/readback/detail/owned_cpu_video_frame_buffer.hpp"
#include "infrastructure/ffmpeg/video_renderer/ffmpeg_video_renderer_backend.hpp"
#include "infrastructure/gpu/readback/readback_statistics.hpp"
#include "infrastructure/notifier/default_notifier.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <gtest/gtest.h>
#include <cstring>

namespace semi::infra::gpu {
namespace {

using contracts::media::VideoPixelFormat;
using contracts::media::VideoFrameBuffer;
using contracts::media::CpuVideoFrameResource;
using contracts::media::resource_as;
using contracts::video_readback::ReadbackOperation;

// Deliberately native-only: no AVFrame, FFmpeg bridge or frame pool.
class NativeFrame final : public VideoFrameBuffer {
public:
    NativeFrame(std::shared_ptr<D3D11Device> device,
                Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
                VideoPixelFormat format, std::uint32_t width, std::uint32_t height,
                std::uint32_t slice)
        : device_(std::move(device)), texture_(std::move(texture)), format_(format),
          width_(width), height_(height), resource_(device_.get(), texture_.Get(), slice) {}

    VideoPixelFormat pixel_format() const noexcept override { return format_; }
    std::uint32_t width() const noexcept override { return width_; }
    std::uint32_t height() const noexcept override { return height_; }
    const contracts::media::VideoFrameResource& resource() const noexcept override { return resource_; }
    D3D11VideoFrameResource& test_resource() { return resource_; }

private:
    std::shared_ptr<D3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    VideoPixelFormat format_;
    std::uint32_t width_, height_;
    D3D11VideoFrameResource resource_;
};

std::vector<std::byte> pattern(VideoPixelFormat format, std::uint32_t width,
                                std::uint32_t height, std::uint32_t slice) {
    const std::size_t sample_bytes = format == VideoPixelFormat::P010 ? 2 : 1;
    const auto pitch = width * sample_bytes;
    std::vector<std::byte> bytes(pitch * (height + height / 2));
    for (std::size_t row = 0; row < height + height / 2; ++row) {
        for (std::size_t col = 0; col < width; ++col) {
            const auto value = slice * 101 + row * 7 + col * 3;
            auto* out = bytes.data() + row * pitch + col * sample_bytes;
            if (sample_bytes == 2) {
                const auto sample = static_cast<std::uint16_t>((value % 1024) << 6);
                std::memcpy(out, &sample, sizeof(sample));
            } else {
                *out = static_cast<std::byte>(value % 256);
            }
        }
    }
    return bytes;
}

class D3D11VideoReadbackTest : public ::testing::TestWithParam<VideoPixelFormat> {
protected:
    void SetUp() override {
        manager = std::make_unique<GpuDeviceManager>();
        auto acquired = manager->acquire_d3d11();
        if (!acquired) GTEST_SKIP() << acquired.error().message;
        device = std::move(*acquired);
        const auto format = GetParam() == VideoPixelFormat::P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        UINT support = 0;
        if (FAILED(device->native_device()->CheckFormatSupport(format, &support)) ||
            !(support & D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
            GTEST_SKIP() << "Native planar texture format unavailable";
        }
    }

    std::unique_ptr<NativeFrame> make_frame(std::uint32_t texture_width = 64,
                                           std::uint32_t texture_height = 32,
                                           std::uint32_t width = 61,
                                           std::uint32_t height = 29,
                                           std::uint32_t slice = 1,
                                           std::optional<VideoPixelFormat> override_format = {}) {
        const auto format = override_format.value_or(GetParam());
        auto first = pattern(format, texture_width, texture_height, 0);
        auto second = pattern(format, texture_width, texture_height, 1);
        const auto pitch = texture_width * (format == VideoPixelFormat::P010 ? 2U : 1U);
        D3D11_SUBRESOURCE_DATA initial[] = {{first.data(), pitch, 0}, {second.data(), pitch, 0}};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = texture_width;
        desc.Height = texture_height;
        desc.MipLevels = 1;
        desc.ArraySize = 2;
        desc.Format = format == VideoPixelFormat::P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        const auto status = device->native_device()->CreateTexture2D(&desc, initial, texture.GetAddressOf());
        EXPECT_EQ(status, S_OK);
        if (FAILED(status)) return {};
        return std::make_unique<NativeFrame>(device, std::move(texture), format, width, height, slice);
    }

    void compare(const VideoFrameBuffer& output, std::uint32_t texture_width,
                 std::uint32_t texture_height, std::uint32_t slice,
                 std::optional<VideoPixelFormat> override_format = {}) {
        const auto format = override_format.value_or(GetParam());
        const auto* cpu = resource_as<CpuVideoFrameResource>(output);
        ASSERT_NE(cpu, nullptr);
        ASSERT_EQ(cpu->plane_count(), 2U);
        const auto expected = pattern(format, texture_width, texture_height, slice);
        const auto pitch = texture_width * (format == VideoPixelFormat::P010 ? 2U : 1U);
        EXPECT_EQ(output.pixel_format(), format);
        for (std::size_t plane = 0; plane < 2; ++plane) {
            const auto view = cpu->plane(plane);
            const auto offset = plane == 0 ? 0 : pitch * texture_height;
            const auto rows = plane == 0 ? output.height() : (output.height() + 1) / 2;
            for (std::size_t row = 0; row < rows; ++row) {
                EXPECT_EQ(std::memcmp(view.data + row * view.stride_bytes,
                                      expected.data() + offset + row * pitch, view.stride_bytes), 0)
                    << "plane=" << plane << " row=" << row;
            }
        }
    }

    std::unique_ptr<GpuDeviceManager> manager;
    std::shared_ptr<D3D11Device> device;
    D3D11VideoReadback readback;
};

TEST_P(D3D11VideoReadbackTest, ReadsSelectedSliceAndCroppedOddDimensionsIntoIndependentMemory) {
    auto input = make_frame();
    ASSERT_NE(input, nullptr);
    auto output = readback.readback(*input);
    ASSERT_TRUE(output.has_value()) << output.error().message;
    EXPECT_EQ((*output)->width(), 61U);
    EXPECT_EQ((*output)->height(), 29U);
    input.reset();
    compare(**output, 64, 32, 1);
}

TEST_P(D3D11VideoReadbackTest, PublishesOneOwnedReportWithErrorsAndCachedStaging) {
    auto notifier = std::make_shared<DefaultNotifier>();
    std::vector<ReadbackStatistics> reports;
    auto subscription = notifier->subscribe<ReadbackStatistics>(
        [&](const ReadbackStatistics& report) { reports.push_back(report); });
    std::uint64_t expected_bytes = 0;
    {
        D3D11VideoReadback measured(notifier);
        auto input = make_frame();
        ASSERT_NE(input, nullptr);
        input->test_resource().array_slice = 99;
        EXPECT_FALSE(measured.readback(*input));
        input->test_resource().array_slice = 1;
        for (int i = 0; i < 2; ++i) {
            auto output = measured.readback(*input);
            ASSERT_TRUE(output) << output.error().message;
            const auto* cpu = resource_as<CpuVideoFrameResource>(**output);
            expected_bytes += cpu->plane(0).size_bytes + cpu->plane(1).size_bytes;
        }
        EXPECT_TRUE(reports.empty()); // No per-frame notifications.
    }
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = reports.front();
    EXPECT_EQ(report.total.calls, 3U);
    EXPECT_EQ(report.successful_calls, 2U);
    EXPECT_EQ(report.failed_calls, 1U);
    EXPECT_EQ(report.copied_bytes, expected_bytes);
    EXPECT_EQ(report.input_format, GetParam());
    EXPECT_FALSE(report.input_changed);
    EXPECT_EQ(report.allocate_cpu.calls, 2U);
    EXPECT_EQ(report.create_staging.calls, 1U);
    EXPECT_EQ(report.wait_context.calls, 2U);
    EXPECT_EQ(report.submit_copy.calls, 2U);
    EXPECT_EQ(report.map.calls, 2U);
    EXPECT_EQ(report.copy_cpu.calls, 2U);
    EXPECT_EQ(report.unmap.calls, 2U);
    const auto phase_sum = report.allocate_cpu.total_ns + report.create_staging.total_ns +
        report.wait_context.total_ns + report.submit_copy.total_ns + report.map.total_ns +
        report.copy_cpu.total_ns + report.unmap.total_ns;
    EXPECT_GE(report.total.total_ns, phase_sum);
    EXPECT_GE(report.total.total_ns, report.total.max_ns);
    EXPECT_GE(report.finished_at_unix_ms, report.started_at_unix_ms);
}

TEST_P(D3D11VideoReadbackTest, SuppressesUnusedReportAndMarksInputChanges) {
    auto notifier = std::make_shared<DefaultNotifier>();
    std::vector<ReadbackStatistics> reports;
    auto subscription = notifier->subscribe<ReadbackStatistics>(
        [&](const ReadbackStatistics& report) { reports.push_back(report); });
    { D3D11VideoReadback unused(notifier); }
    EXPECT_TRUE(reports.empty());
    {
        D3D11VideoReadback measured(notifier);
        auto input = make_frame();
        ASSERT_NE(input, nullptr);
        ASSERT_TRUE(measured.readback(*input));
        auto resized = make_frame(80, 48, 77, 45);
        ASSERT_NE(resized, nullptr);
        ASSERT_TRUE(measured.readback(*resized));
    }
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].total.calls, 2U);
    EXPECT_EQ(reports[0].create_staging.calls, 2U);
    EXPECT_EQ(reports[0].input_width, 61U);
    EXPECT_EQ(reports[0].input_height, 29U);
    EXPECT_TRUE(reports[0].input_changed);
}

TEST_P(D3D11VideoReadbackTest, RepeatedFramesAndLayoutChangesHaveNoStalePixels) {
    auto first = make_frame(64, 32, 64, 32, 0);
    ASSERT_NE(first, nullptr);
    auto old_output = readback.readback(*first);
    ASSERT_TRUE(old_output.has_value());
    auto second = make_frame(64, 32, 61, 29, 1);
    ASSERT_NE(second, nullptr);
    auto next = readback.readback(*second);
    ASSERT_TRUE(next.has_value());
    compare(**next, 64, 32, 1);
    compare(**old_output, 64, 32, 0);
    auto resized = make_frame(80, 48, 77, 45, 0);
    ASSERT_NE(resized, nullptr);
    auto result = readback.readback(*resized);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    compare(**result, 80, 48, 0);
}

TEST_P(D3D11VideoReadbackTest, RejectsWrongDeviceAndSliceThenRemainsUsable) {
    auto input = make_frame();
    ASSERT_NE(input, nullptr);
    input->test_resource().array_slice = 2;
    auto bad_slice = readback.readback(*input);
    ASSERT_FALSE(bad_slice.has_value());
    EXPECT_EQ(bad_slice.error().operation, ReadbackOperation::Validate);
    input->test_resource().array_slice = 1;
    GpuDeviceManager other_manager;
    auto other = other_manager.acquire_d3d11();
    ASSERT_TRUE(other.has_value());
    input->test_resource().device = other->get();
    EXPECT_FALSE(readback.readback(*input).has_value());
    input->test_resource().device = device.get();
    auto valid = readback.readback(*input);
    ASSERT_TRUE(valid.has_value());
    compare(**valid, 64, 32, 1);
}

TEST_P(D3D11VideoReadbackTest, RebuildsStagingWhenSourceDeviceChanges) {
    auto input = make_frame();
    ASSERT_NE(input, nullptr);
    auto first = readback.readback(*input);
    ASSERT_TRUE(first.has_value());
    std::weak_ptr<D3D11Device> old_device = device;
    input.reset();
    device.reset();
    manager.reset();
    // The cache may retain native COM objects, but not the old wrapper or mutex.
    EXPECT_TRUE(old_device.expired());
    GpuDeviceManager other_manager;
    auto other = other_manager.acquire_d3d11();
    ASSERT_TRUE(other.has_value());
    device = std::move(*other);
    input = make_frame();
    ASSERT_NE(input, nullptr);
    auto next = readback.readback(*input);
    ASSERT_TRUE(next.has_value()) << next.error().message;
    compare(**next, 64, 32, 1);
}

TEST_P(D3D11VideoReadbackTest, RebuildsStagingWhenPixelFormatChanges) {
    const auto other = GetParam() == VideoPixelFormat::Nv12 ? VideoPixelFormat::P010 : VideoPixelFormat::Nv12;
    const auto native = other == VideoPixelFormat::P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    UINT support = 0;
    if (FAILED(device->native_device()->CheckFormatSupport(native, &support)) ||
        !(support & D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
        GTEST_SKIP() << "Second planar format unavailable";
    }
    auto first = make_frame();
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(readback.readback(*first).has_value());
    auto second = make_frame(64, 32, 61, 29, 0, other);
    ASSERT_NE(second, nullptr);
    auto result = readback.readback(*second);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    compare(**result, 64, 32, 0, other);
}

TEST_P(D3D11VideoReadbackTest, FeedsNativeOnlyFrameThroughInjectedReadbackAndSwscale) {
    auto input = make_frame(64, 32, 64, 32);
    ASSERT_NE(input, nullptr);
    ffmpeg::video_renderer::FfmpegVideoRendererBackend renderer(std::make_shared<D3D11VideoReadback>());
    ASSERT_TRUE(renderer.configure({}).has_value());
    contracts::media::DecodedVideo frame{.buffer = std::move(input), .pts_us = 4567};
    auto rendered = renderer.render(frame);
    ASSERT_TRUE(rendered.has_value()) << rendered.error().message;
    EXPECT_EQ(rendered->width, 64U);
    EXPECT_EQ(rendered->height, 32U);
    EXPECT_EQ(rendered->pts_us, 4567);
    EXPECT_EQ(rendered->pixels.size(), 64U * 32U * 4U);
}

INSTANTIATE_TEST_SUITE_P(PlanarFormats, D3D11VideoReadbackTest,
                        ::testing::Values(VideoPixelFormat::Nv12, VideoPixelFormat::P010));

TEST(D3D11VideoReadbackErrorTest, RejectsCpuResources) {
    detail::OwnedCpuVideoFrameBuffer input(VideoPixelFormat::Nv12, 8, 8);
    D3D11VideoReadback reader;
    auto result = reader.readback(input);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().operation, ReadbackOperation::Validate);
}

} // namespace
} // namespace semi::infra::gpu
