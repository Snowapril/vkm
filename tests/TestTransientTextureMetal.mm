#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#include "TestTransientTextureShared.hpp"

#include <memory>

namespace
{
    struct MetalTransientTextureFixture
    {
        std::unique_ptr<vkm::VkmDriverMetal> driver;
        vkm::VkmInitResult initResult;

        MetalTransientTextureFixture()
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
        // These cases record and submit GPU work, so the driver has to be torn down properly --
        // an undestroyed one leaves its queue and deferred reclaimer live and hangs a later test.
        ~MetalTransientTextureFixture()
        {
            if (driver)
            {
                driver->destroy();
            }
            driver.reset();
        }
    };
}

TEST_CASE("Metal transient texture - lands in the Transient pool and reports what it got")
{
    MetalTransientTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runTransientTextureCreationTest(f.driver.get());
}

TEST_CASE("Metal transient texture - an unbackable request is downgraded, not rejected")
{
    MetalTransientTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runTransientTextureIncompatibleFlagsTest(f.driver.get());
}

TEST_CASE("Metal transient texture - a heap placement hint does not cost it its tile memory")
{
    MetalTransientTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runTransientHeapHintTest(f.driver.get());
}

TEST_CASE("Metal transient buffer - the flag is texture-only and dropped")
{
    MetalTransientTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runTransientBufferFlagIsDroppedTest(f.driver.get());
}

TEST_CASE("Metal transient texture - a render pass with a transient depth attachment")
{
    MetalTransientTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runTransientDepthRenderPassTest(f.driver.get(), /*storeDepth=*/false);
}

TEST_CASE("Metal transient texture - storing a transient attachment is coerced away")
{
    MetalTransientTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runTransientDepthRenderPassTest(f.driver.get(), /*storeDepth=*/true);
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
