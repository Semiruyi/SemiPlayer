#include "infrastructure/gpu/readback/d3d11_video_readback.hpp"
#include "infrastructure/gpu/readback/readback_statistics.hpp"
#include "infrastructure/notifier/notifier.hpp"
#include "infrastructure/log/log.hpp"

#include <algorithm>
#include <chrono>

#define SEMI_LOG_TAG "gpu_readback"

#if defined(_WIN32)
#include "infrastructure/gpu/d3d11/d3d11_device.hpp"
#include "infrastructure/gpu/d3d11/d3d11_video_frame_resource.hpp"
#include "infrastructure/gpu/readback/detail/owned_cpu_video_frame_buffer.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#endif

namespace semi::infra::gpu {
namespace {
using contracts::video_readback::ReadbackError;
using contracts::video_readback::ReadbackOperation;

using Clock = std::chrono::steady_clock;

std::int64_t unix_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

class PhaseTimer {
public:
    explicit PhaseTimer(ReadbackTiming* timing) noexcept : timing_(timing) {
        if (timing_) started_ = Clock::now();
    }
    ~PhaseTimer() {
        if (!timing_) return;
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started_).count());
        ++timing_->calls;
        timing_->total_ns += elapsed;
        timing_->max_ns = std::max(timing_->max_ns, elapsed);
    }
    PhaseTimer(const PhaseTimer&) = delete;
    PhaseTimer& operator=(const PhaseTimer&) = delete;
private:
    ReadbackTiming* timing_;
    Clock::time_point started_;
};

struct CallOutcome {
    ReadbackStatistics* report;
    bool succeeded = false;
    ~CallOutcome() {
        if (!report) return;
        if (succeeded) ++report->successful_calls;
        else ++report->failed_calls;
    }
};

std::unexpected<ReadbackError> error(ReadbackOperation operation, std::int32_t code,
                                     const char* message) {
    return std::unexpected(ReadbackError{operation, code, message});
}

#if defined(_WIN32)
struct UnmapOnExit {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    ReadbackTiming* timing;
    ~UnmapOnExit() {
        PhaseTimer timer(timing);
        context->Unmap(texture, 0);
    }
};
#endif
} // namespace

struct D3D11VideoReadback::Impl {
    std::shared_ptr<Notifier> notifier;
    ReadbackStatistics statistics;
    Clock::time_point started;
#if defined(_WIN32)
    // COM owners keep the native identity stable. Never cache borrowed
    // D3D11Device wrapper pointers or frame/resource descriptions.
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC layout{};
#endif
};

D3D11VideoReadback::D3D11VideoReadback(std::shared_ptr<Notifier> notifier)
    : impl_(std::make_unique<Impl>()) {
    impl_->notifier = std::move(notifier);
    impl_->statistics.backend = "d3d11";
}

D3D11VideoReadback::~D3D11VideoReadback() {
    if (!impl_->notifier || impl_->statistics.total.calls == 0) return;
    auto& report = impl_->statistics;
    report.finished_at_unix_ms = unix_ms();
    report.observed_duration_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - impl_->started).count());
    try {
        (void)impl_->notifier->send(report);
    } catch (...) {
        SEMI_LOG_WARN("cannot submit readback statistics");
    }
}

std::expected<std::unique_ptr<const contracts::media::VideoFrameBuffer>, ReadbackError>
D3D11VideoReadback::readback(const contracts::media::VideoFrameBuffer& input) {
    auto* report = impl_->notifier ? &impl_->statistics : nullptr;
    PhaseTimer total_timer(report ? &report->total : nullptr);
    CallOutcome outcome{report};
    if (report) {
        if (report->total.calls == 0) {
            impl_->started = Clock::now();
            report->started_at_unix_ms = unix_ms();
            report->input_format = input.pixel_format();
            report->input_width = input.width();
            report->input_height = input.height();
        } else if (report->input_format != input.pixel_format() ||
                   report->input_width != input.width() || report->input_height != input.height()) {
            report->input_changed = true;
        }
    }
#if defined(_WIN32)
    using contracts::media::VideoPixelFormat;
    const auto* source = contracts::media::resource_as<D3D11VideoFrameResource>(input);
    if (!source || !source->device || !source->texture) {
        return error(ReadbackOperation::Validate, 0, "D3D11 readback requires a valid D3D11 resource");
    }
    DXGI_FORMAT format;
    if (input.pixel_format() == VideoPixelFormat::Nv12) format = DXGI_FORMAT_NV12;
    else if (input.pixel_format() == VideoPixelFormat::P010) format = DXGI_FORMAT_P010;
    else return error(ReadbackOperation::Validate, 0, "D3D11 readback supports NV12 and P010 only");

    D3D11_TEXTURE2D_DESC layout{};
    source->texture->GetDesc(&layout);
    Microsoft::WRL::ComPtr<ID3D11Device> texture_device;
    source->texture->GetDevice(texture_device.GetAddressOf());
    if (texture_device.Get() != source->device->native_device() || layout.Format != format ||
        source->array_slice >= layout.ArraySize || layout.MipLevels != 1 ||
        layout.SampleDesc.Count != 1 || (layout.Width % 2) != 0 || (layout.Height % 2) != 0 ||
        input.width() == 0 || input.height() == 0 ||
        input.width() > layout.Width || input.height() > layout.Height) {
        return error(ReadbackOperation::Validate, 0, "D3D11 frame device, format, slice or dimensions are invalid");
    }

    try {
        std::unique_ptr<detail::OwnedCpuVideoFrameBuffer> output;
        {
            PhaseTimer timer(report ? &report->allocate_cpu : nullptr);
            output = std::make_unique<detail::OwnedCpuVideoFrameBuffer>(
                input.pixel_format(), input.width(), input.height());
        }
        if (!impl_->staging || impl_->device.Get() != texture_device.Get() ||
            impl_->layout.Width != layout.Width || impl_->layout.Height != layout.Height ||
            impl_->layout.Format != layout.Format) {
            PhaseTimer timer(report ? &report->create_staging : nullptr);
            D3D11_TEXTURE2D_DESC desc = layout;
            desc.ArraySize = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
            const HRESULT status = texture_device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf());
            if (FAILED(status)) {
                return error(ReadbackOperation::CreateStaging, static_cast<std::int32_t>(status),
                             "D3D11 CreateTexture2D for readback staging failed");
            }
            impl_->staging = std::move(staging);
            impl_->device = std::move(texture_device);
            impl_->layout = layout;
        }

        auto lock = [&] {
            PhaseTimer timer(report ? &report->wait_context : nullptr);
            return source->device->lock_context();
        }();
        auto* context = source->device->native_context();
        {
            PhaseTimer timer(report ? &report->submit_copy : nullptr);
            context->CopySubresourceRegion(impl_->staging.Get(), 0, 0, 0, 0,
                                          source->texture, source->array_slice, nullptr);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT status;
        {
            PhaseTimer timer(report ? &report->map : nullptr);
            status = context->Map(impl_->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        }
        if (FAILED(status)) {
            return error(ReadbackOperation::Map, static_cast<std::int32_t>(status),
                         "D3D11 Map for readback failed");
        }
        UnmapOnExit unmap{context, impl_->staging.Get(), report ? &report->unmap : nullptr};
        const auto* cpu = contracts::media::resource_as<contracts::media::CpuVideoFrameResource>(*output);
        const auto maximum = std::numeric_limits<std::size_t>::max();
        const auto total_rows = static_cast<std::size_t>(layout.Height) + layout.Height / 2;
        if (!mapped.pData || mapped.RowPitch < cpu->plane(1).stride_bytes ||
            total_rows > maximum / mapped.RowPitch) {
            return error(ReadbackOperation::Map, 0, "D3D11 mapped layout has invalid pointer or row pitch");
        }
        const auto* y = static_cast<const std::byte*>(mapped.pData);
        const auto* uv = y + static_cast<std::size_t>(mapped.RowPitch) * layout.Height;
        {
            PhaseTimer timer(report ? &report->copy_cpu : nullptr);
            for (std::size_t plane = 0; plane < 2; ++plane) {
                const auto view = cpu->plane(plane);
                auto destination = output->writable_plane(plane);
                const auto rows = plane == 0 ? input.height()
                    : (static_cast<std::size_t>(input.height()) + 1) / 2;
                const auto* data = plane == 0 ? y : uv;
                for (std::size_t row = 0; row < rows; ++row) {
                    std::memcpy(destination.data() + row * view.stride_bytes,
                                data + row * mapped.RowPitch, view.stride_bytes);
                }
            }
        }
        if (report) report->copied_bytes += cpu->plane(0).size_bytes + cpu->plane(1).size_bytes;
        outcome.succeeded = true;
        return std::unique_ptr<const contracts::media::VideoFrameBuffer>(std::move(output));
    } catch (const std::bad_alloc&) {
        return error(ReadbackOperation::Allocate, static_cast<std::int32_t>(E_OUTOFMEMORY),
                     "allocating CPU readback image failed");
    } catch (const std::length_error&) {
        return error(ReadbackOperation::Allocate, static_cast<std::int32_t>(E_OUTOFMEMORY),
                     "CPU readback image exceeds allocation limit");
    } catch (const std::invalid_argument& exception) {
        return error(ReadbackOperation::Validate, 0, exception.what());
    }
#else
    (void)input;
    return error(ReadbackOperation::Validate, 0, "D3D11 readback is not supported on this platform");
#endif
}

} // namespace semi::infra::gpu
