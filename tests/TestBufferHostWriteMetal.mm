#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#include "TestBufferHostWriteShared.hpp"

#include <memory>

namespace
{
    struct MetalBufferHostWriteFixture
    {
        std::unique_ptr<vkm::VkmDriverMetal> driver;
        vkm::VkmInitResult initResult;

        MetalBufferHostWriteFixture()
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

TEST_CASE("Metal buffer - the host-write and staging upload paths agree")
{
    MetalBufferHostWriteFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runBufferHostWriteTest(f.driver.get());
}

TEST_CASE("Metal buffer - GPU addresses follow the device capability")
{
    MetalBufferHostWriteFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    vkmtest::runBufferGpuAddressTest(f.driver.get());
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
