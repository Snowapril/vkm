#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/engine.h>
#include <vkm/platform/common/app_delegate.h>

#include <functional>

#ifdef VKM_USE_VULKAN_API
#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
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

    driver.reset();
    glfwDestroyWindow(window);
    glfwTerminate();
}

namespace {
// Per tests/CLAUDE.md: no direct graphics API calls -- this only drives the engine's own
// AppDelegate/loopInner() surface, exactly like a real sample would.
struct NullAppDelegate : vkm::AppDelegate {
    void postDriverReady() override {}
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
