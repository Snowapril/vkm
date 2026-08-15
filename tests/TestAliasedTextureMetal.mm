#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#include "TestAliasedTextureShared.hpp"

#include <memory>

namespace
{
    struct MetalAliasedTextureFixture
    {
        std::unique_ptr<vkm::VkmDriverMetal> driver;
        vkm::VkmInitResult initResult;

        MetalAliasedTextureFixture()
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
        ~MetalAliasedTextureFixture()
        {
            if (driver)
            {
                driver->destroy();
            }
            driver.reset();
        }
    };
}

TEST_CASE("Metal aliasable texture - the flag survives only where it can be served")
{
    MetalAliasedTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runAliasableFlagSanitizerTest(f.driver.get());
}

TEST_CASE("Metal aliasable buffer - the flag is texture-only and dropped")
{
    MetalAliasedTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runAliasableBufferFlagIsDroppedTest(f.driver.get());
}

TEST_CASE("Metal aliased textures - disjoint lifetimes share bytes")
{
    MetalAliasedTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runAliasedPlacementTest(f.driver.get());
}

TEST_CASE("Metal aliased textures - overlapping lifetimes keep their own bytes")
{
    MetalAliasedTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runOverlappingLifetimeIsRefusedTest(f.driver.get());
}

TEST_CASE("Metal aliased textures - an undeclared attachment widens the lifetime")
{
    MetalAliasedTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runUndeclaredAliasedUseTest(f.driver.get());
}

TEST_CASE("Metal aliased textures - each reads back its own contents" * doctest::timeout(10.0))
{
    MetalAliasedTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runAliasedPixelTest(f.driver.get());
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
