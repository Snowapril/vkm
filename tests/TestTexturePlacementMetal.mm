#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_USE_METAL_API) && defined(VKM_PLATFORM_APPLE)

#import <Metal/Metal.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/texture.h>

/*
* Metal-only on purpose, with no TestXxxShared.hpp counterpart: Metal is the only backend with
* an engine-owned texture heap. Vulkan's Heap textures are placed by VMA's internal
* suballocator, which reports nothing the engine can assert on, and WebGPU has no placement API
* at all. A shared body would be an empty shell on two of three backends -- so this does not
* need "fixing" into the usual shape.
*/

namespace
{
    struct TexturePlacementFixture {
        id<MTLDevice> device = nil;
        vkm::VkmDriverMetal* driver = nullptr;
        vkm::VkmInitResult initResult;
        TexturePlacementFixture() {
            device = MTLCreateSystemDefaultDevice();
            if (device == nil) {
                initResult = vkm::VkmInitResult{vkm::VkmInitResultCode::HardwareUnsupported, "No Metal device available on this system."};
                return;
            }
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = new vkm::VkmDriverMetal(device);
            initResult = driver->initialize(&opts);
        }
        ~TexturePlacementFixture() {
            delete driver;
        }
    };

    constexpr uint32_t kTextureExtent = 512;
    constexpr uint64_t kTextureBytes = uint64_t(kTextureExtent) * kTextureExtent * 4; // RGBA8

    vkm::VkmTexture* createTexture(vkm::VkmDriverBase* driver, vkm::VkmMemoryPlacementHint placementHint,
                                   vkm::VkmResourceCreateInfo flags, const char* debugName)
    {
        vkm::VkmTextureInfo info{};
        info._flags = flags;
        info._extent = {kTextureExtent, kTextureExtent, 1};
        info._numMipLevels = 1;
        info._numArrayLayers = 1;
        info._format = vkm::VkmFormat::R8G8B8A8_UNORM;
        info._placementHint = placementHint;
        info._debugName = debugName;
        return driver->newTexture(info);
    }
} // namespace

/*
* A texture that ignores _placementHint and creates itself directly leaves the driver with no
* heap block at all, so _hasPoolStats stays false and _poolUsedBytes flat -- which is what
* these two CHECKs pin.
*/
TEST_CASE("VkmTextureMetal - a Heap texture is placed in the shared MTLHeap") {
    TexturePlacementFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    /*
    * Shader read+write, and deliberately NOT AllowTransferDst. Two flags are excluded on
    * purpose, and both exclusions are load-bearing:
    *  - an attachment would be forced committed by the Auto policy;
    *  - AllowTransferDst makes shouldUseHostWritableTexture say yes on a unified-memory device,
    *    which puts the texture in MTLStorageModeShared and therefore also forces committed
    *    (the heap is Private). That interaction is the subject of the next test; here it would
    *    just stop this one from reaching the heap path at all.
    */
    const vkm::VkmResourceCreateInfo devicePrivateFlags =
        vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowShaderWrite;

    const vkm::VkmGpuMemoryStats before = f.driver->getGpuMemoryStats();

    vkm::VkmTexture* heapTexture = createTexture(f.driver, vkm::VkmMemoryPlacementHint::Heap,
                                                 devicePrivateFlags, "HeapPlacedTexture");
    REQUIRE(heapTexture != nullptr);
    REQUIRE_FALSE(heapTexture->isHostWritable()); // guards the premise above
    CHECK(heapTexture->getHandle().isValid());

    const vkm::VkmGpuMemoryStats after = f.driver->getGpuMemoryStats();

    CHECK(after._hasPoolStats);
    CHECK(after._poolUsedBytes >= before._poolUsedBytes + kTextureBytes);

    // Alignment comes from heapTextureSizeAndAlignWithDescriptor:. A zero here would mean the
    // descriptor reported no heap footprint yet was heap-placed anyway.
    CHECK(heapTexture->getMemoryAlignment() > 0);

    f.driver->getRenderResourcePool()->releaseResource(heapTexture->getHandle());
}

/*
* The heap is MTLStorageModePrivate and Metal requires a placed resource's storage mode to
* match its heap's, so a host-writable (Shared) texture must stay committed even when Heap is
* asked for explicitly. Without the override this is a Metal validation failure rather than a
* wrong value -- which is why the suite runs with the debug layer on.
*/
TEST_CASE("VkmTextureMetal - a host-writable texture stays committed despite an explicit Heap hint") {
    TexturePlacementFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    // shouldUseHostWritableTexture only says yes on a unified-memory device; elsewhere the
    // texture is Private and legitimately heap-placeable, so there is nothing to assert.
    if (!f.driver->hasUnifiedMemory()) {
        MESSAGE("Skipped: this device does not have unified memory, so no texture is host-writable.");
        return;
    }

    const vkm::VkmResourceCreateInfo uploadFlags =
        vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowTransferDst;

    const vkm::VkmGpuMemoryStats before = f.driver->getGpuMemoryStats();

    vkm::VkmTexture* texture = createTexture(f.driver, vkm::VkmMemoryPlacementHint::Heap,
                                             uploadFlags, "HostWritableHeapHintTexture");
    REQUIRE(texture != nullptr);
    REQUIRE(texture->isHostWritable());

    const vkm::VkmGpuMemoryStats after = f.driver->getGpuMemoryStats();

    // It went somewhere other than the heap: pool usage did not grow by this texture's size.
    CHECK(after._poolUsedBytes < before._poolUsedBytes + kTextureBytes);

    f.driver->getRenderResourcePool()->releaseResource(texture->getHandle());
}

/*
* Auto keeps attachments committed (few, long-lived, resize-churny), which is the one usage
* rule the allocator cannot infer for itself. Pins that policy so a future simplification of
* shouldUseCommittedTexture cannot drop it silently.
*/
TEST_CASE("VkmTextureMetal - Auto keeps a color attachment committed") {
    TexturePlacementFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    const vkm::VkmResourceCreateInfo attachmentFlags =
        vkm::VkmResourceCreateInfo::AllowColorAttachment | vkm::VkmResourceCreateInfo::AllowShaderRead;

    const vkm::VkmGpuMemoryStats before = f.driver->getGpuMemoryStats();

    vkm::VkmTexture* attachment = createTexture(f.driver, vkm::VkmMemoryPlacementHint::Auto,
                                                attachmentFlags, "AutoAttachmentTexture");
    REQUIRE(attachment != nullptr);

    const vkm::VkmGpuMemoryStats after = f.driver->getGpuMemoryStats();
    CHECK(after._poolUsedBytes < before._poolUsedBytes + kTextureBytes);

    f.driver->getRenderResourcePool()->releaseResource(attachment->getHandle());
}

#endif // VKM_USE_METAL_API && VKM_PLATFORM_APPLE
