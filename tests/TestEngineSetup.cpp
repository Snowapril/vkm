#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/gpu_offset_allocator.h>
#include <vkm/renderer/backend/common/render_resource.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/engine.h>
#include <vkm/platform/common/app_delegate.h>

#include <functional>

#ifdef VKM_USE_VULKAN_API
#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_staging_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_sampler.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture_view.h>
#include <vkm/renderer/backend/vulkan/vulkan_buffer_view.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_queue.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <glm/vec2.hpp>
#endif

#ifdef VKM_USE_WEBGPU_API
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_command_queue.h>
#endif

// ---------------------------------------------------------------------------
// Pure-logic tests (no GPU required)
// ---------------------------------------------------------------------------

TEST_CASE("VkmResourceHandle - equality and validity") {
    vkm::VkmResourceHandle a{42, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Texture};
    vkm::VkmResourceHandle b{42, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Texture};
    vkm::VkmResourceHandle c{99, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Buffer};

    CHECK(a == b);
    CHECK(a != c);
    CHECK(a.isValid());
    CHECK(a.isPooledResource());
    CHECK_FALSE(vkm::VKM_INVALID_RESOURCE_HANDLE.isValid());
    CHECK_FALSE(vkm::VKM_INVALID_RESOURCE_HANDLE.isPooledResource());
}

TEST_CASE("VkmResourceHandle - Sampler/TextureView/BufferView are distinct valid pooled types") {
    vkm::VkmResourceHandle sampler{1, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Sampler};
    vkm::VkmResourceHandle textureView{1, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::TextureView};
    vkm::VkmResourceHandle bufferView{1, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::BufferView};

    CHECK(sampler.isValid());
    CHECK(textureView.isValid());
    CHECK(bufferView.isValid());
    CHECK(sampler.isPooledResource());
    CHECK(textureView.isPooledResource());
    CHECK(bufferView.isPooledResource());
    CHECK(sampler != textureView);
    CHECK(textureView != bufferView);
    CHECK(sampler != bufferView);
}

TEST_CASE("VkmResourceHandle - std::hash is consistent and distinguishes distinct handles") {
    std::hash<vkm::VkmResourceHandle> hasher;
    vkm::VkmResourceHandle a{1, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Texture};
    vkm::VkmResourceHandle b{2, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Texture};
    CHECK(hasher(a) == hasher(a));
    CHECK(hasher(a) != hasher(b));
}

TEST_CASE("VkmFormat - hasDepth returns true only for depth-bearing formats") {
    CHECK(vkm::hasDepth(vkm::VkmFormat::D32_SFLOAT));
    CHECK(vkm::hasDepth(vkm::VkmFormat::D24_UNORM_S8_UINT));
    CHECK(vkm::hasDepth(vkm::VkmFormat::D32_SFLOAT_S8_UINT));
    CHECK_FALSE(vkm::hasDepth(vkm::VkmFormat::R8G8B8A8_UNORM));
    CHECK_FALSE(vkm::hasDepth(vkm::VkmFormat::R8G8B8A8_SRGB));
    CHECK_FALSE(vkm::hasDepth(vkm::VkmFormat::R16G16B16A16_SFLOAT));
    CHECK_FALSE(vkm::hasDepth(vkm::VkmFormat::R32G32B32A32_SFLOAT));
    CHECK_FALSE(vkm::hasDepth(vkm::VkmFormat::Undefined));
}

TEST_CASE("VkmFormat - hasStencil returns true only for stencil-bearing formats") {
    CHECK(vkm::hasStencil(vkm::VkmFormat::D24_UNORM_S8_UINT));
    CHECK(vkm::hasStencil(vkm::VkmFormat::D32_SFLOAT_S8_UINT));
    CHECK_FALSE(vkm::hasStencil(vkm::VkmFormat::D32_SFLOAT));
    CHECK_FALSE(vkm::hasStencil(vkm::VkmFormat::R8G8B8A8_UNORM));
    CHECK_FALSE(vkm::hasStencil(vkm::VkmFormat::Undefined));
}

namespace {
class MockGpuEventTimeline : public vkm::VkmGpuEventTimelineBase {
public:
    MockGpuEventTimeline() : vkm::VkmGpuEventTimelineBase(nullptr) {}
    uint64_t queryLastCompletedTimeline() override { return _lastCompletedCachedTimeline; }
    void waitIdle(const uint64_t) override {}
    void setLastCompleted(uint64_t value) { _lastCompletedCachedTimeline = value; }
};

class MockRenderResource : public vkm::VkmRenderResource {
public:
    MockRenderResource() : vkm::VkmRenderResource(nullptr) {}
    vkm::VkmResourceType getResourceType() const override { return vkm::VkmResourceType::Buffer; }
    void setDebugName(const char*) override {}
};
} // namespace

TEST_CASE("VkmGpuEventTimelineBase - allocateGpuEventTimelineObject increments monotonically") {
    MockGpuEventTimeline tl;
    CHECK(tl.getLastAllocatedTimeline() == 0);

    auto obj1 = tl.allocateGpuEventTimelineObject();
    CHECK(obj1._gpuEventTimeline == &tl);
    CHECK(obj1._timelineValue == 1);
    CHECK(tl.getLastAllocatedTimeline() == 1);

    auto obj2 = tl.allocateGpuEventTimelineObject();
    CHECK(obj2._timelineValue == 2);
    CHECK(obj2._timelineValue > obj1._timelineValue);
    CHECK(tl.getLastAllocatedTimeline() == 2);
}

TEST_CASE("VkmRenderResource - recordUsage/getLastUsage/hasAnyPendingUsage tracks per-queue timelines") {
    MockRenderResource resource;
    CHECK_FALSE(resource.hasAnyPendingUsage());

    MockGpuEventTimeline timeline;
    auto usage = timeline.allocateGpuEventTimelineObject(); // timelineValue == 1

    resource.recordUsage(usage);
    CHECK(resource.getLastUsage(usage._gpuEventTimeline)._timelineValue == 1);
    CHECK(resource.hasAnyPendingUsage()); // completed cache still 0 < 1

    timeline.setLastCompleted(1); // simulate the GPU catching up
    CHECK_FALSE(resource.hasAnyPendingUsage());

    // A later usage on the same queue overwrites the earlier one -- only the latest matters.
    auto usage2 = timeline.allocateGpuEventTimelineObject(); // timelineValue == 2
    resource.recordUsage(usage2);
    CHECK(resource.getLastUsage(usage2._gpuEventTimeline)._timelineValue == 2);
    CHECK(resource.hasAnyPendingUsage()); // completed cache is 1 < 2

    timeline.setLastCompleted(2);
    CHECK_FALSE(resource.hasAnyPendingUsage());

    // A second, independent timeline (simulating a second queue instance of the same
    // type) must not clobber the first's tracked entry -- this is the actual regression
    // test for the bug this phase fixes (previously, a fixed 3-slot array indexed only by
    // queue TYPE could not distinguish two instances of the same type).
    MockGpuEventTimeline timeline2;
    auto usage3 = timeline2.allocateGpuEventTimelineObject(); // timelineValue == 1 on timeline2
    resource.recordUsage(usage3);
    CHECK(resource.getLastUsage(usage3._gpuEventTimeline)._timelineValue == 1);
    CHECK(resource.getLastUsage(usage2._gpuEventTimeline)._timelineValue == 2); // first timeline's entry untouched
    CHECK(resource.hasAnyPendingUsage()); // timeline2 not yet completed

    timeline2.setLastCompleted(1);
    CHECK_FALSE(resource.hasAnyPendingUsage()); // both timelines now complete
}

namespace {
// Minimal VkmDriverBase/VkmCommandQueueBase/VkmBuffer stand-ins so
// VkmDeferredResourceReclaimer can be exercised without a real GPU backend. Only the
// initialize()/allocateBuffer() paths actually used by the test below are meaningful --
// the rest are inert stubs required purely to satisfy pure virtuals.
class FakeCommandQueue : public vkm::VkmCommandQueueBase {
public:
    FakeCommandQueue(vkm::VkmDriverBase* driver) : vkm::VkmCommandQueueBase(driver) {}
    void setDebugName(const char*) override {}
    vkm::VkmGpuEventTimelineObject submit(const vkm::CommandSubmitInfo&) override { return {}; }
protected:
    bool initializeInner() override { return true; }
};

class FakeDriver : public vkm::VkmDriverBase {
protected:
    vkm::VkmInitResult initializeInner(const vkm::VkmEngineLaunchOptions*) override
    {
        return vkm::VkmInitResult{vkm::VkmInitResultCode::Success, ""};
    }
    void destroyInner() override {}
    vkm::VkmTexture* newTextureInner() override { return nullptr; }
    vkm::VkmBuffer* newBufferInner() override { return nullptr; }
    vkm::VkmStagingBuffer* newStagingBufferInner() override { return nullptr; }
    vkm::VkmSampler* newSamplerInner() override { return nullptr; }
    vkm::VkmTextureView* newTextureViewInner() override { return nullptr; }
    vkm::VkmBufferView* newBufferViewInner() override { return nullptr; }
    vkm::VkmSwapChainBase* newSwapChainInner() override { return nullptr; }
    vkm::VkmCommandQueueBase* newCommandQueueInner() override { return new FakeCommandQueue(this); }
};

class MockBuffer : public vkm::VkmBuffer {
public:
    MockBuffer(vkm::VkmDriverBase* driver) : vkm::VkmBuffer(driver) {}
    bool initialize(vkm::VkmResourceHandle, const vkm::VkmBufferInfo&) override { return true; }
    bool overrideExternalHandle(void*) override { return true; }
    void setDebugName(const char*) override {}
};
} // namespace

TEST_CASE("VkmDeferredResourceReclaimer - pollOnce releases only once every recorded usage completes") {
    FakeDriver driver;
    vkm::VkmInitResult initResult = driver.initialize(nullptr);
    REQUIRE(initResult.code == vkm::VkmInitResultCode::Success);

    // A fresh, never-started() reclaimer -- driven purely via pollOnce(), never the real
    // background thread the driver's own _deferredReclaimer already started internally.
    vkm::VkmDeferredResourceReclaimer testReclaimer(&driver);

    vkm::VkmResourceHandle handle = driver.getRenderResourcePool()->allocateBuffer(new MockBuffer(&driver));
    REQUIRE(handle.isValid());

    MockGpuEventTimeline timeline;
    auto usage = timeline.allocateGpuEventTimelineObject(); // timelineValue == 1
    driver.getRenderResourcePool()->getResource<vkm::VkmBuffer>(handle)->recordUsage(usage);

    testReclaimer.requestRelease(handle);

    testReclaimer.pollOnce();
    CHECK(driver.getRenderResourcePool()->getResource<vkm::VkmBuffer>(handle) != nullptr); // still pending

    timeline.setLastCompleted(1);
    testReclaimer.pollOnce();
    CHECK(driver.getRenderResourcePool()->getResource<vkm::VkmBuffer>(handle) == nullptr); // now released

    driver.destroy();
}

TEST_CASE("VkmOffsetAllocator - allocate returns valid, non-overlapping ranges") {
    vkm::VkmOffsetAllocator allocator(1024);

    auto a = allocator.allocate(64, 1);
    auto b = allocator.allocate(128, 1);
    CHECK(a.isValid());
    CHECK(b.isValid());
    CHECK(a._offset != b._offset);

    allocator.free(a);
    allocator.free(b);
}

TEST_CASE("VkmOffsetAllocator - alignment padding returns correctly aligned offsets") {
    vkm::VkmOffsetAllocator allocator(4096);

    // Force an unaligned starting point, then request a 256-aligned allocation.
    auto filler = allocator.allocate(3, 1);
    CHECK(filler.isValid());

    auto aligned = allocator.allocate(64, 256);
    CHECK(aligned.isValid());
    CHECK((aligned._offset % 256) == 0);

    allocator.free(aligned);
    allocator.free(filler);
}

TEST_CASE("VkmOffsetAllocator - free then reallocate reuses freed space") {
    vkm::VkmOffsetAllocator allocator(256);

    auto a = allocator.allocate(256, 1);
    CHECK(a.isValid());

    // Pool is full; a second allocation of any size must fail.
    auto b = allocator.allocate(1, 1);
    CHECK_FALSE(b.isValid());

    allocator.free(a);

    // Freed space must be reusable.
    auto c = allocator.allocate(256, 1);
    CHECK(c.isValid());
    allocator.free(c);
}

TEST_CASE("VkmOffsetAllocator - allocation exceeding pool size fails") {
    vkm::VkmOffsetAllocator allocator(128);
    auto a = allocator.allocate(256, 1);
    CHECK_FALSE(a.isValid());
}

// ---------------------------------------------------------------------------
// Vulkan headless driver + swapchain tests
// ---------------------------------------------------------------------------

#ifdef VKM_USE_VULKAN_API

struct VulkanDriverFixture {
    std::unique_ptr<vkm::VkmDriverVulkan> driver;
    vkm::VkmInitResult initResult;
    VulkanDriverFixture() {
        glfwInit();
        vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
        driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
        initResult = driver->initialize(&opts);
    }
    ~VulkanDriverFixture() {
        // destroy() must run before the driver object is deleted: it tears down
        // driver-owned pools (VmaAllocator, buffer pools, deferred reclaimer thread) that
        // pooled resources' destructors depend on. Skipping it (as this fixture previously
        // did) is safe only by accident when nothing needs ordered teardown.
        if (driver)
        {
            driver->destroy();
        }
        driver.reset();
        glfwTerminate();
    }
};

TEST_CASE("VkmDriverVulkan - initialization succeeds") {
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    SUBCASE("render resource pool is available") {
        CHECK(f.driver->getRenderResourcePool() != nullptr);
    }
    SUBCASE("graphics command queue is created") {
        CHECK(f.driver->getCommandQueue(vkm::VkmCommandQueueType::Graphics, 0) != nullptr);
    }
    SUBCASE("compute command queue is created") {
        CHECK(f.driver->getCommandQueue(vkm::VkmCommandQueueType::Compute, 0) != nullptr);
    }
    SUBCASE("Vulkan handles are valid after init") {
        CHECK(f.driver->getDevice() != VK_NULL_HANDLE);
        CHECK(f.driver->getPhysicalDevice() != VK_NULL_HANDLE);
        CHECK(f.driver->getInstance() != VK_NULL_HANDLE);
    }
    SUBCASE("CommandBufferReusable capability flag is set on Vulkan") {
        CHECK((f.driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::CommandBufferReusable) != 0);
    }
}

TEST_CASE("VkmDriverVulkan - VmaAllocator is created on init") {
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    CHECK(f.driver->getVmaAllocator() != nullptr);
}

TEST_CASE("VkmDriverVulkan - newBuffer/newStagingBuffer/newSampler create valid resources") {
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    SUBCASE("committed buffer") {
        vkm::VkmBufferInfo info{};
        info._flags = vkm::VkmResourceCreateInfo::AllowShaderRead;
        info._size = 256;
        info._placementHint = vkm::VkmMemoryPlacementHint::ForceCommitted;
        vkm::VkmBuffer* buffer = f.driver->newBuffer(info);
        REQUIRE(buffer != nullptr);
        CHECK(buffer->getHandle().isValid());
        auto* vkBuffer = f.driver->getRenderResourcePool()->getResource<vkm::VkmBufferVulkan>(buffer->getHandle());
        REQUIRE(vkBuffer != nullptr);
        CHECK(vkBuffer->getBuffer() != VK_NULL_HANDLE);
        // Never GPU-used (no timeline recorded), so an immediate release is correct here --
        // requestRelease() is only needed once a resource may have been submitted.
        f.driver->getRenderResourcePool()->releaseResource(buffer->getHandle());
    }

    SUBCASE("pooled buffer") {
        vkm::VkmBufferInfo info{};
        info._flags = vkm::VkmResourceCreateInfo::AllowShaderRead;
        info._size = 256;
        info._placementHint = vkm::VkmMemoryPlacementHint::ForcePooled;
        vkm::VkmBuffer* buffer = f.driver->newBuffer(info);
        REQUIRE(buffer != nullptr);
        auto* vkBuffer = f.driver->getRenderResourcePool()->getResource<vkm::VkmBufferVulkan>(buffer->getHandle());
        REQUIRE(vkBuffer != nullptr);
        CHECK(vkBuffer->getBuffer() != VK_NULL_HANDLE);
        f.driver->getRenderResourcePool()->releaseResource(buffer->getHandle());
    }

    SUBCASE("staging buffer is host-mapped") {
        vkm::VkmStagingBufferInfo info{};
        info._flags = vkm::VkmResourceCreateInfo::AllowTransferSrc;
        info._size = 256;
        vkm::VkmStagingBuffer* stagingBuffer = f.driver->newStagingBuffer(info);
        REQUIRE(stagingBuffer != nullptr);
        CHECK(stagingBuffer->map() != nullptr);
        f.driver->getRenderResourcePool()->releaseResource(stagingBuffer->getHandle());
    }

    SUBCASE("sampler") {
        vkm::VkmSamplerInfo info{};
        vkm::VkmSampler* sampler = f.driver->newSampler(info);
        REQUIRE(sampler != nullptr);
        auto* vkSampler = f.driver->getRenderResourcePool()->getResource<vkm::VkmSamplerVulkan>(sampler->getHandle());
        REQUIRE(vkSampler != nullptr);
        CHECK(vkSampler->getSampler() != VK_NULL_HANDLE);
        f.driver->getRenderResourcePool()->releaseResource(sampler->getHandle());
    }
}

TEST_CASE("VkmDriverVulkan - texture view and buffer view resolve their parent resource") {
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    vkm::VkmTextureInfo textureInfo{};
    textureInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderRead;
    textureInfo._extent = {4, 4, 1};
    textureInfo._numMipLevels = 1;
    textureInfo._numArrayLayers = 1;
    textureInfo._format = vkm::VkmFormat::R8G8B8A8_UNORM;
    vkm::VkmTexture* texture = f.driver->newTexture(textureInfo);
    REQUIRE(texture != nullptr);

    vkm::VkmTextureViewInfo viewInfo{};
    viewInfo._texture = texture->getHandle();
    viewInfo._numMipLevels = 1;
    viewInfo._numArrayLayers = 1;
    vkm::VkmTextureView* textureView = f.driver->newTextureView(viewInfo);
    REQUIRE(textureView != nullptr);
    auto* vkTextureView = f.driver->getRenderResourcePool()->getResource<vkm::VkmTextureViewVulkan>(textureView->getHandle());
    REQUIRE(vkTextureView != nullptr);
    CHECK(vkTextureView->getImageView() != VK_NULL_HANDLE);

    vkm::VkmBufferInfo bufferInfo{};
    bufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderRead;
    bufferInfo._size = 256;
    vkm::VkmBuffer* buffer = f.driver->newBuffer(bufferInfo);
    REQUIRE(buffer != nullptr);

    vkm::VkmBufferViewInfo bufferViewInfo{};
    bufferViewInfo._buffer = buffer->getHandle();
    bufferViewInfo._offset = 0;
    bufferViewInfo._size = 256;
    vkm::VkmBufferView* bufferView = f.driver->newBufferView(bufferViewInfo);
    REQUIRE(bufferView != nullptr);
    auto* vkBufferView = f.driver->getRenderResourcePool()->getResource<vkm::VkmBufferViewVulkan>(bufferView->getHandle());
    REQUIRE(vkBufferView != nullptr);
    CHECK(vkBufferView->getOffset() == 0);
    CHECK(vkBufferView->getSize() == 256);

    // Release views before their parent resources -- a view must not outlive what it views.
    vkm::VkmRenderResourcePool* pool = f.driver->getRenderResourcePool();
    pool->releaseResource(bufferView->getHandle());
    pool->releaseResource(buffer->getHandle());
    pool->releaseResource(textureView->getHandle());
    pool->releaseResource(texture->getHandle());
}

TEST_CASE("VkmDriverVulkan - graphics queue exposes a valid VkQueue handle") {
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    auto* queue = static_cast<vkm::VkmCommandQueueVulkan*>(
        f.driver->getCommandQueue(vkm::VkmCommandQueueType::Graphics, 0));
    REQUIRE(queue != nullptr);
    CHECK(queue->getVkQueue() != VK_NULL_HANDLE);
}

TEST_CASE("VkmSwapChainVulkan - created and initialized with a hidden GLFW window") {
    glfwInit();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(256, 256, "UnitTest", nullptr, nullptr);
    if (window == nullptr) {
        MESSAGE("Skipping: glfwCreateWindow failed (no display server available in this environment).");
        glfwTerminate();
        return;
    }

    vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };

    std::unique_ptr<vkm::VkmDriverVulkan> driver(new vkm::VkmDriverVulkan());
    vkm::VkmInitResult initResult = driver->initialize(&opts);
    if (initResult.code == vkm::VkmInitResultCode::HardwareUnsupported) {
        MESSAGE("Skipping: " << initResult.reason);
        glfwDestroyWindow(window);
        glfwTerminate();
        return;
    }
    REQUIRE_MESSAGE(initResult.code == vkm::VkmInitResultCode::Success, initResult.reason);

    {
        // unique_ptr destroyed before driver — correct order: swapchain uses driver's resource pool
        std::unique_ptr<vkm::VkmSwapChainBase> sc(driver->newSwapChain());
        REQUIRE(sc != nullptr);
        REQUIRE(sc->initialize(vkm::VkmWindowInfo{ 256, 256, "UnitTest", window }));
        CHECK(sc->getExtent() == glm::uvec2(256u, 256u));
    }

    driver->destroy();
    driver.reset();
    glfwDestroyWindow(window);
    glfwTerminate();
}

namespace {
// Per tests/CLAUDE.md: no direct graphics API calls -- this only drives the engine's own
// AppDelegate/loopInner() surface, exactly like a real sample would.
struct NullAppDelegate : vkm::AppDelegate {
    void postDriverReady(vkm::VkmEngine*) override {}
    void preShutdown() override {}
    void update(const double) override {}
    void render(vkm::VkmRenderGraph*, vkm::VkmResourceHandle) override {}
    const char* getAppName() const override { return "ImGuiSmokeTest"; }
};
} // namespace

#if defined(VKM_ENABLE_IMGUI)
TEST_CASE("VkmEngine - ImGui renderer initializes and survives one loopInner() tick") {
    glfwInit();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(256, 256, "UnitTest", nullptr, nullptr);
    REQUIRE(window != nullptr);

    vkm::VkmEngine engine(new vkm::VkmDriverVulkan());
    REQUIRE(engine.initializeEngine(new NullAppDelegate(), vkm::VkmEngineLaunchOptions{ .enableValidationLayer = false }));

    vkm::VkmInitResult initResult = engine.initializeBackendDriver();
    if (initResult.code == vkm::VkmInitResultCode::HardwareUnsupported) {
        MESSAGE("Skipping: " << initResult.reason);
        glfwDestroyWindow(window);
        glfwTerminate();
        return;
    }
    REQUIRE_MESSAGE(initResult.code == vkm::VkmInitResultCode::Success, initResult.reason);

    engine.addSwapChain(vkm::VkmWindowInfo{ 256, 256, "UnitTest", window });
    CHECK_NOTHROW(engine.loopInner(0.016));

    engine.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
}
#endif // VKM_ENABLE_IMGUI

#endif // VKM_USE_VULKAN_API

// ---------------------------------------------------------------------------
// WebGPU headless driver tests
// ---------------------------------------------------------------------------

#ifdef VKM_USE_WEBGPU_API

struct WebGPUDriverFixture {
    vkm::VkmDriverWebGPU* driver = nullptr;
    vkm::VkmInitResult initResult;
    WebGPUDriverFixture() {
        vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
        driver = new vkm::VkmDriverWebGPU();
        initResult = driver->initialize(&opts);
    }
    ~WebGPUDriverFixture() { delete driver; }
};

TEST_CASE("VkmDriverWebGPU - initialization succeeds") {
    WebGPUDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    SUBCASE("WGPUInstance is exposed and non-null") {
        CHECK(f.driver->getInstance() != nullptr);
    }
    SUBCASE("WGPUAdapter is exposed and non-null") {
        CHECK(f.driver->getAdapter() != nullptr);
    }
    SUBCASE("WGPUDevice is exposed and non-null") {
        CHECK(f.driver->getDevice() != nullptr);
    }
    SUBCASE("WGPUQueue is exposed and non-null") {
        CHECK(f.driver->getQueue() != nullptr);
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
    SUBCASE("driver capability flags are None on WebGPU") {
        CHECK(f.driver->getDriverCapabilityFlags() == vkm::VkmDriverCapabilityFlags::None);
    }
}

TEST_CASE("VkmDriverWebGPU - graphics queue exposes a valid WGPUQueue") {
    WebGPUDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    auto* queue = static_cast<vkm::VkmCommandQueueWebGPU*>(
        f.driver->getCommandQueue(vkm::VkmCommandQueueType::Graphics, 0));
    REQUIRE(queue != nullptr);
    CHECK(queue->getWGPUQueue() != nullptr);
}

#endif // VKM_USE_WEBGPU_API
