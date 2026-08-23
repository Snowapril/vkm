#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#include "TestSparseTextureShared.hpp"

#include <memory>

namespace
{
    struct MetalSparseTextureFixture
    {
        std::unique_ptr<vkm::VkmDriverMetal> driver;
        vkm::VkmInitResult initResult;

        MetalSparseTextureFixture()
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
        ~MetalSparseTextureFixture()
        {
            if (driver)
            {
                driver->destroy();
            }
            driver.reset();
        }
    };
}

TEST_CASE("Metal sparse texture - granted with a usable mip tail, or honestly refused")
{
    MetalSparseTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runSparseTextureTest(f.driver.get());
}

TEST_CASE("Metal sparse texture - loses to flags that decide backing memory another way")
{
    MetalSparseTextureFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runSparseFlagSanitizeTest(f.driver.get());
}

#endif // defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)
