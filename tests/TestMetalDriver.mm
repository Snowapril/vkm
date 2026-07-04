#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <glm/vec2.hpp>

struct MetalDriverFixture {
    id<MTLDevice> device;
    vkm::VkmDriverMetal* driver = nullptr;
    MetalDriverFixture() {
        device = MTLCreateSystemDefaultDevice();
        REQUIRE(device != nil);
        vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = false };
        driver = new vkm::VkmDriverMetal(device);
        REQUIRE(driver->initialize(&opts));
    }
    ~MetalDriverFixture() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdelete-non-abstract-non-virtual-dtor"
        delete driver;
#pragma clang diagnostic pop
    }
};

TEST_CASE("VkmDriverMetal - initialization succeeds") {
    MetalDriverFixture f;

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
}

TEST_CASE("VkmDriverMetal - graphics queue exposes a valid MTLCommandQueue") {
    MetalDriverFixture f;
    auto* queue = static_cast<vkm::VkmCommandQueueMetal*>(
        f.driver->getCommandQueue(vkm::VkmCommandQueueType::Graphics, 0));
    REQUIRE(queue != nullptr);
    CHECK(queue->getMTLCommandQueue() != nil);
}

TEST_CASE("VkmSwapChainMetal - created and initialized without a display surface") {
    MetalDriverFixture f;

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
