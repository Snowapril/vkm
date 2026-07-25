#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#include "TestCubemapTextureShared.hpp"

#include <memory>

namespace
{
    struct MetalCubemapFixture
    {
        std::unique_ptr<vkm::VkmDriverMetal> driver;
        vkm::VkmInitResult initResult;

        MetalCubemapFixture()
        {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (device == nil)
            {
                initResult = vkm::VkmInitResult{vkm::VkmInitResultCode::HardwareUnsupported,
                                                "No Metal device available on this system."};
                return;
            }
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::make_unique<vkm::VkmDriverMetal>(device);
            initResult = driver->initialize(&opts);
        }
    };
}

TEST_CASE("Metal cubemap - six faces upload, read back, and register bindless")
{
    MetalCubemapFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runCubemapTextureTest(f.driver.get());
}

TEST_CASE("Metal skybox - cubemap faces land in the right screen regions")
{
    MetalCubemapFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runSkyboxRenderTest(f.driver.get());
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
