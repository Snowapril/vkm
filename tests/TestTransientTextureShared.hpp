#ifndef TEST_TRANSIENT_TEXTURE_SHARED_HPP
#define TEST_TRANSIENT_TEXTURE_SHARED_HPP

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/texture.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace vkmtest
{
    inline constexpr uint32_t kTransientTextureSize = 64;

    inline vkm::VkmTexture* createTransientTexture(vkm::VkmDriverBase* driver, vkm::VkmResourceCreateInfo flags,
                                                   vkm::VkmFormat format, const char* debugName,
                                                   vkm::VkmMemoryPlacementHint placementHint =
                                                       vkm::VkmMemoryPlacementHint::Auto)
    {
        vkm::VkmTextureInfo textureInfo{};
        textureInfo._flags = flags;
        textureInfo._placementHint = placementHint;
        textureInfo._extent = glm::uvec3(kTransientTextureSize, kTransientTextureSize, 1);
        textureInfo._numMipLevels = 1;
        textureInfo._numArrayLayers = 1;
        textureInfo._format = format;
        textureInfo._debugName = debugName;
        return driver->newTexture(textureInfo);
    }

    /*
    * @brief A transient texture lands in the Transient sub-pool and reports whatever the
    * backend actually allocated.
    * @details The pool type follows the *request*, so it is asserted on every backend; the
    * grant is a device property (a Vulkan device may offer no lazily-allocated memory type),
    * so it is logged rather than asserted and the memory tag is checked against the outcome.
    */
    inline void runTransientTextureCreationTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);
        vkm::VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();

        const auto checkTransientTexture = [&](vkm::VkmResourceCreateInfo flags, vkm::VkmFormat format,
                                               const char* debugName) {
            vkm::VkmTexture* texture = createTransientTexture(driver, flags | vkm::VkmResourceCreateInfo::Transient,
                                                              format, debugName);
            REQUIRE(texture != nullptr);
            const vkm::VkmResourceHandle handle = texture->getHandle();
            CHECK(handle.isValid());
            CHECK(handle.poolType == vkm::VkmResourcePoolType::Transient);
            CHECK(handle.isPooledResource());
            // The flag survived sanitization, which is what put the handle in that sub-pool.
            CHECK((texture->getTextureInfo()._flags & vkm::VkmResourceCreateInfo::Transient) != 0);
            // Built up first: doctest's MESSAGE binds its own operator tighter than +, so an
            // inline concatenation would be parsed against the message builder instead.
            const std::string grantMessage = std::string("transient granted for ") + debugName + ": " +
                                             (texture->isTransient() ? "yes" : "no");
            MESSAGE(grantMessage);

            const std::optional<vkm::VkmResourceMemoryTag> tag = resourcePool->getResourceMemoryTag(handle);
            REQUIRE(tag.has_value());
            CHECK(tag->requestedSize > 0);
            if (texture->isTransient())
            {
                // Nothing is committed, so nothing may be reported -- otherwise the memory
                // report grows by an attachment that costs no device memory at all.
                CHECK(tag->allocatedSize == 0);
                CHECK(tag->metadata == "transient");
            }
            else
            {
                // The fallback is an ordinary attachment and must account for itself as one.
                CHECK(tag->metadata.empty());
            }

            resourcePool->releaseResource(handle);
        };

        SUBCASE("depth attachment")
        {
            checkTransientTexture(vkm::VkmResourceCreateInfo::AllowDepthStencilAttachment,
                                  vkm::VkmFormat::D32_SFLOAT, "TransientDepth");
        }

        SUBCASE("color attachment")
        {
            checkTransientTexture(vkm::VkmResourceCreateInfo::AllowColorAttachment,
                                  driver->getSwapChainColorFormat(), "TransientColor");
        }
    }

    /*
    * @brief A Transient request the APIs cannot back is downgraded, not rejected.
    * @details Both Vulkan (VUID-VkImageCreateInfo-usage-00963/00966) and Metal forbid every
    * non-attachment usage on a tile-only resource. With validation layers on, this is the case
    * that fails loudly if VkmDriverBase ever stops clearing the flag.
    */
    inline void runTransientTextureIncompatibleFlagsTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);
        vkm::VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();

        const auto checkDowngraded = [&](vkm::VkmResourceCreateInfo flags, const char* debugName) {
            vkm::VkmTexture* texture = createTransientTexture(driver, flags, driver->getSwapChainColorFormat(),
                                                              debugName);
            REQUIRE(texture != nullptr);
            CHECK(texture->getHandle().poolType == vkm::VkmResourcePoolType::Default);
            CHECK_FALSE(texture->isTransient());
            // Cleared before any backend saw it -- that is what keeps the descriptor legal.
            CHECK((texture->getTextureInfo()._flags & vkm::VkmResourceCreateInfo::Transient) == 0);
            resourcePool->releaseResource(texture->getHandle());
        };

        SUBCASE("sampling a tile-only attachment is impossible")
        {
            checkDowngraded(vkm::VkmResourceCreateInfo::AllowColorAttachment |
                            vkm::VkmResourceCreateInfo::AllowShaderRead |
                            vkm::VkmResourceCreateInfo::Transient, "TransientSampled");
        }

        SUBCASE("blitting a tile-only attachment is impossible")
        {
            checkDowngraded(vkm::VkmResourceCreateInfo::AllowColorAttachment |
                            vkm::VkmResourceCreateInfo::AllowTransferSrc |
                            vkm::VkmResourceCreateInfo::Transient, "TransientTransferSrc");
        }

        SUBCASE("a presentable attachment cannot be tile-only")
        {
            checkDowngraded(vkm::VkmResourceCreateInfo::AllowColorAttachment |
                            vkm::VkmResourceCreateInfo::AllowPresent |
                            vkm::VkmResourceCreateInfo::Transient, "TransientPresent");
        }
    }

    /*
    * @brief A Transient texture asked to be heap-placed still gets tile memory, or is downgraded
    * -- never silently charged for real memory.
    * @details The two requests pull in opposite directions and the backends resolve it differently.
    * Vulkan can honour both: VMA suballocates from a LAZILY_ALLOCATED block. Metal cannot honour
    * both at once and keeps the transient half, which is the one that saves memory:
    * newHeapWithDescriptor: rejects MTLStorageModeMemoryless outright ("Requested storage mode is
    * not allowed for Heaps"), and placing a memoryless texture in the Private heap the engine does
    * have is "The requested storage mode does not match the heap's mode" -- both hard validation
    * assertions. heapTextureSizeAndAlignWithDescriptor: is no help either: it answers a *non-zero*
    * footprint for a memoryless descriptor, so the generic is-this-heap-placeable test does not
    * filter one out. Nothing is lost by committing it: a committed memoryless texture already
    * reports allocatedSize 0, so there is no memory a heap could save.
    */
    inline void runTransientHeapHintTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);
        vkm::VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();

        vkm::VkmTexture* texture = createTransientTexture(
            driver, vkm::VkmResourceCreateInfo::AllowDepthStencilAttachment | vkm::VkmResourceCreateInfo::Transient,
            vkm::VkmFormat::D32_SFLOAT, "TransientHeapHinted", vkm::VkmMemoryPlacementHint::Heap);
        REQUIRE(texture != nullptr);

        const vkm::VkmResourceHandle handle = texture->getHandle();
        CHECK(handle.poolType == vkm::VkmResourcePoolType::Transient);

        const std::optional<vkm::VkmResourceMemoryTag> tag = resourcePool->getResourceMemoryTag(handle);
        REQUIRE(tag.has_value());
        if (texture->isTransient())
        {
            CHECK(tag->allocatedSize == 0);
            CHECK(tag->metadata == "transient");
        }
        else
        {
            // Downgraded to an ordinary attachment, which then has to account for itself as one.
            CHECK(tag->metadata.empty());
        }
        resourcePool->releaseResource(handle);
    }

    // Neither Vulkan nor Metal has a transient buffer, so the flag is dropped there outright.
    inline void runTransientBufferFlagIsDroppedTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);

        vkm::VkmBufferInfo bufferInfo{};
        bufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::Transient;
        bufferInfo._size = 256;
        bufferInfo._debugName = "TransientBuffer";
        vkm::VkmBuffer* buffer = driver->newBuffer(bufferInfo);
        REQUIRE(buffer != nullptr);

        CHECK(buffer->getHandle().poolType == vkm::VkmResourcePoolType::Default);
        CHECK((buffer->getBufferInfo()._flags & vkm::VkmResourceCreateInfo::Transient) == 0);
        driver->getRenderResourcePool()->releaseResource(buffer->getHandle());
    }

    /*
    * @brief Clears a stored color target alongside a transient depth attachment and reads the
    * color back.
    * @details The assertions are the smaller half of what this covers: with validation layers
    * on, a memoryless texture that reached the encoder with the wrong storage mode, usage or
    * store action aborts the run before any CHECK is reached.
    *
    * `storeDepth` drives the guard case: asking to load and store a transient attachment is
    * illegal, and VkmCommandBufferBase::beginRenderPass must coerce both actions rather than
    * let the backend see them. The frame completing with the right pixels is the observable
    * proof, since VKM_DEBUG_ERROR is not catchable from doctest.
    */
    inline void runTransientDepthRenderPassTest(vkm::VkmDriverBase* driver, bool storeDepth)
    {
        REQUIRE(driver != nullptr);
        vkm::VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();

        vkm::VkmTexture* colorTarget = createTransientTexture(
            driver, vkm::VkmResourceCreateInfo::AllowColorAttachment | vkm::VkmResourceCreateInfo::AllowTransferSrc,
            driver->getSwapChainColorFormat(), "TransientPassColor");
        REQUIRE(colorTarget != nullptr);

        vkm::VkmTexture* depthTarget = createTransientTexture(
            driver, vkm::VkmResourceCreateInfo::AllowDepthStencilAttachment | vkm::VkmResourceCreateInfo::Transient,
            vkm::VkmFormat::D32_SFLOAT, "TransientPassDepth");
        REQUIRE(depthTarget != nullptr);

        vkm::VkmFrameBufferDescriptor fbDesc{};
        fbDesc._width = kTransientTextureSize;
        fbDesc._height = kTransientTextureSize;
        fbDesc._renderPass._colorAttachmentCount = 1;
        fbDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        fbDesc._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
        fbDesc._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
        fbDesc._renderPass._colorAttachments[0]._clearColors[0] = 1.0f;
        fbDesc._renderPass._colorAttachments[0]._clearColors[1] = 1.0f;
        fbDesc._renderPass._colorAttachments[0]._clearColors[2] = 1.0f;
        fbDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;
        fbDesc._colorAttachments[0] = colorTarget->getHandle();

        vkm::VkmDepthStencilAttachmentDescriptor depthAttachment{};
        depthAttachment._attachmentId = 1;
        // Load is only asked for when the grant actually came through, so the guard is what
        // resolves it. On a device that fell back, loading an attachment nothing has written
        // would be a validation complaint of its own rather than a test of this feature.
        depthAttachment._loadAction = (storeDepth && depthTarget->isTransient()) ? vkm::VkmLoadAction::Load
                                                                                 : vkm::VkmLoadAction::Clear;
        depthAttachment._storeAction = storeDepth ? vkm::VkmStoreAction::Store : vkm::VkmStoreAction::DontCare;
        depthAttachment._clearDepth = 1.0f;
        depthAttachment._clearStencil = 0;
        fbDesc._renderPass._depthStencilAttachment = depthAttachment;
        fbDesc._depthStencilAttachment = depthTarget->getHandle();

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* subGraph = renderGraph.beginGraphicsSubGraph(fbDesc, "TransientDepthPass");
        // Nothing to draw: the clear alone is what has to reach the transient attachment safely.
        subGraph->setRenderCallback([](vkm::VkmCommandBufferBase*) {});
        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();

        const vkm::VkmTextureReadbackResult readback = driver->readbackTexture(colorTarget->getHandle());
        REQUIRE(readback.channels > 0);
        REQUIRE_FALSE(readback.pixels.empty());
        const size_t centerTexel =
            (static_cast<size_t>(readback.height / 2) * readback.width + readback.width / 2) * readback.channels;
        REQUIRE(readback.pixels.size() > centerTexel);
        // A white clear: the depth attachment must not have disturbed the colour target,
        // whichever actions the caller asked for.
        CHECK(readback.pixels[centerTexel] >= 254);

        resourcePool->releaseResource(depthTarget->getHandle());
        resourcePool->releaseResource(colorTarget->getHandle());
    }
} // namespace vkmtest

#endif // TEST_TRANSIENT_TEXTURE_SHARED_HPP
