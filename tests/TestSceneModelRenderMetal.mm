#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#include "TestSceneModelRenderShared.hpp"

#include <memory>

namespace
{
    struct MetalSceneModelFixture
    {
        std::unique_ptr<vkm::VkmDriverMetal> driver;
        vkm::VkmInitResult initResult;

        MetalSceneModelFixture()
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

TEST_CASE("Metal scene model - an imported glTF mesh renders through the bindless path")
{
    MetalSceneModelFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runSceneModelRenderTest(f.driver.get());
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
