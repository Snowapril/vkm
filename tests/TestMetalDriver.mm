#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <glm/vec2.hpp>

struct MetalDriverFixture {
    id<MTLDevice> device = nil;
    vkm::VkmDriverMetal* driver = nullptr;
    vkm::VkmInitResult initResult;
    MetalDriverFixture() {
        device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            initResult = vkm::VkmInitResult{vkm::VkmInitResultCode::HardwareUnsupported, "No Metal device available on this system."};
            return;
        }
        vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
        driver = new vkm::VkmDriverMetal(device);
        initResult = driver->initialize(&opts);
    }
    ~MetalDriverFixture() {
        delete driver;
    }
};

TEST_CASE("VkmDriverMetal - initialization succeeds") {
    MetalDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    SUBCASE("MTLDevice is exposed and non-nil") {
        CHECK(f.driver->getMTLDevice() != nil);
    }
    SUBCASE("render resource pool is available") {
        CHECK(f.driver->getRenderResourcePool() != nullptr);
    }
    SUBCASE("graphics command queue is created") {
        CHECK(f.driver->getCommandQueue(vkm::VkmCommandQueueType::Graphics, 0) != nullptr);
    }
    SUBCASE("compute command queue is created") {
        CHECK(f.driver->getCommandQueue(vkm::VkmCommandQueueType::Compute, 0) != nullptr);
    }
    SUBCASE("CommandBufferReusable capability flag is not set on Metal") {
        // Metal command buffers are single-use; the flag must be absent
        CHECK((f.driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::CommandBufferReusable) == 0);
    }
    SUBCASE("RayTracing implies BufferDeviceAddress") {
        // Not a redundant pair. An acceleration structure is built from geometry addressed by
        // device address (Vulkan's VK_KHR_acceleration_structure requires the
        // bufferDeviceAddress feature outright), so a backend that claims to trace rays but
        // cannot report a buffer's GPU address has nothing to build one from. Asserted as an
        // implication rather than a value because the answer is per-device: MoltenVK exposes no
        // RT extensions at all, and lavapipe only does from Mesa 24.1.
        const uint32_t flags = static_cast<uint32_t>(f.driver->getDriverCapabilityFlags());
        const bool rayTracing = (flags & static_cast<uint32_t>(vkm::VkmDriverCapabilityFlags::RayTracing)) != 0u;
        const bool deviceAddress =
            (flags & static_cast<uint32_t>(vkm::VkmDriverCapabilityFlags::BufferDeviceAddress)) != 0u;
        // Logged, not asserted: this is how a CI run reports which platforms can host Phase 5
        // at all, which is a property of the runner rather than of the code.
        if (rayTracing) { MESSAGE("RayTracing capability on this device: yes"); }
        else            { MESSAGE("RayTracing capability on this device: no"); }
        CHECK((!rayTracing || deviceAddress));
    }
}

TEST_CASE("VkmDriverMetal - graphics queue exposes a valid MTLCommandQueue") {
    MetalDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    auto* queue = static_cast<vkm::VkmCommandQueueMetal*>(
        f.driver->getCommandQueue(vkm::VkmCommandQueueType::Graphics, 0));
    REQUIRE(queue != nullptr);
    CHECK(queue->getMTLCommandQueue() != nil);
}

TEST_CASE("VkmDriverMetal - getGpuMemoryStats reports device allocation that grows with a real buffer") {
    MetalDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    const vkm::VkmGpuMemoryStats before = f.driver->getGpuMemoryStats();
    REQUIRE(before._hasDeviceStats);
    CHECK(before._deviceBudgetBytes > 0);

    // 16 MiB is large enough that the device-reported figure has to move, without being big
    // enough to matter on any Metal-capable machine.
    constexpr uint64_t kBufferSize = 16ull * 1024 * 1024;
    vkm::VkmBufferInfo bufferInfo{};
    bufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderWrite | vkm::VkmResourceCreateInfo::AllowTransferDst;
    bufferInfo._size = kBufferSize;
    bufferInfo._placementHint = vkm::VkmMemoryPlacementHint::ForceCommitted;
    bufferInfo._debugName = "GpuMemoryStatsProbe";
    vkm::VkmBuffer* buffer = f.driver->newBuffer(bufferInfo);
    REQUIRE(buffer != nullptr);

    const vkm::VkmGpuMemoryStats after = f.driver->getGpuMemoryStats();
    CHECK(after._deviceAllocatedBytes >= before._deviceAllocatedBytes + kBufferSize);

    // The engine's own tracked total must have moved too -- the two sides of the
    // tracked-vs-actual comparison the memory inspector shows.
    const vkm::VkmResourceCategoryUsage usage =
        f.driver->getRenderResourcePool()->getCategoryMemoryUsage(vkm::VkmResourceType::Buffer);
    CHECK(usage.totalRequestedBytes >= kBufferSize);
    CHECK(after._deviceAllocatedBytes >= usage.totalAllocatedBytes);

    f.driver->getRenderResourcePool()->releaseResource(buffer->getHandle());
}

TEST_CASE("VkmSwapChainMetal - created and initialized without a display surface") {
    MetalDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    // Metal's createSwapChain ignores the windowHandle argument — it creates
    // deferred VkmTextureMetal slots that get their handles via overrideCurrentDrawable.
    // Passing nullptr is intentional for headless testing.
    std::unique_ptr<vkm::VkmSwapChainBase> sc(f.driver->newSwapChain());
    REQUIRE(sc != nullptr);
    REQUIRE(sc->initialize(vkm::VkmWindowInfo{ 64, 64, "UnitTest", nullptr }));

    SUBCASE("extent matches the requested dimensions") {
        CHECK(sc->getExtent() == glm::uvec2(64u, 64u));
    }
    SUBCASE("back buffer count equals FRAME_BUFFER_COUNT") {
        CHECK(static_cast<uint32_t>(vkm::FRAME_BUFFER_COUNT) == 3u);
    }
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
