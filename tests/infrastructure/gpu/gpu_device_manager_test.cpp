#include "infrastructure/gpu/gpu_device_manager.hpp"

#include <gtest/gtest.h>

#if defined(_WIN32)
#include "infrastructure/gpu/d3d11/d3d11_device.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <future>
#include <vector>

namespace semi::infra::gpu {
namespace {

TEST(GpuDeviceManagerTest, ReusesHardwareDeviceAndMatchingContext) {
    GpuDeviceManager manager;
    auto first = manager.acquire_d3d11();
    if (!first) GTEST_SKIP() << first.error().message << " HRESULT=" << first.error().native_code;
    auto second = manager.acquire_d3d11();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->get(), second->get());
    ASSERT_NE((*first)->native_device(), nullptr);
    ASSERT_NE((*first)->native_context(), nullptr);
    EXPECT_EQ((*first)->native_device()->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED, 0U);
    EXPECT_NE((*first)->native_device()->GetCreationFlags() & D3D11_CREATE_DEVICE_VIDEO_SUPPORT, 0U);
    Microsoft::WRL::ComPtr<ID3D11Device> context_device;
    (*first)->native_context()->GetDevice(context_device.GetAddressOf());
    EXPECT_EQ(context_device.Get(), (*first)->native_device());
}

TEST(GpuDeviceManagerTest, DeviceOutlivesCacheAndReleasesWithLastOwner) {
    auto manager = std::make_unique<GpuDeviceManager>();
    auto result = manager->acquire_d3d11();
    if (!result) GTEST_SKIP() << result.error().message << " HRESULT=" << result.error().native_code;
    auto owner = std::move(*result);
    std::weak_ptr<D3D11Device> weak = owner;
    manager.reset();
    ASSERT_FALSE(weak.expired());
    EXPECT_EQ(owner->native_device()->GetDeviceRemovedReason(), S_OK);
    owner.reset();
    EXPECT_TRUE(weak.expired());
}

TEST(GpuDeviceManagerTest, ConcurrentFirstRequestsShareOneDevice) {
    GpuDeviceManager manager;
    std::promise<void> start;
    auto ready = start.get_future().share();
    using Result = std::expected<std::shared_ptr<D3D11Device>, GpuDeviceError>;
    std::vector<std::future<Result>> requests;
    for (int i = 0; i < 8; ++i) {
        requests.push_back(std::async(std::launch::async, [&manager, ready] {
            ready.wait();
            return manager.acquire_d3d11();
        }));
    }
    start.set_value();
    std::vector<Result> results;
    for (auto& request : requests) results.push_back(request.get());
    if (!results.front()) GTEST_SKIP() << results.front().error().message;
    for (auto& result : results) {
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->get(), results.front()->get());
    }
}

} // namespace
} // namespace semi::infra::gpu
#else
TEST(GpuDeviceManagerTest, ReportsUnsupportedBackendWithoutPlatformSpecificTypes) {
    semi::infra::gpu::GpuDeviceManager manager;
    auto result = manager.acquire_d3d11();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().native_code, 0);
    EXPECT_FALSE(result.error().message.empty());
}
#endif
