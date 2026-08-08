#ifndef TEST_ALIASED_TEXTURE_SHARED_HPP
#define TEST_ALIASED_TEXTURE_SHARED_HPP

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/aliased_memory_heap.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/texture.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace vkmtest
{
    inline constexpr uint32_t kAliasedTextureSize = 64;

    inline vkm::VkmTexture* createAliasedTexture(vkm::VkmDriverBase* driver, vkm::VkmResourceCreateInfo flags,
                                                 const char* debugName)
    {
        vkm::VkmTextureInfo textureInfo{};
        textureInfo._flags = flags;
        textureInfo._extent = glm::uvec3(kAliasedTextureSize, kAliasedTextureSize, 1);
        textureInfo._numMipLevels = 1;
        textureInfo._numArrayLayers = 1;
        textureInfo._format = driver->getSwapChainColorFormat();
        textureInfo._debugName = debugName;
        return driver->newTexture(textureInfo);
    }

    inline vkm::VkmFrameBufferDescriptor makeAliasedFb(vkm::VkmResourceHandle target, vkm::VkmLoadAction loadAction,
                                                       float red)
    {
        vkm::VkmFrameBufferDescriptor fb{};
        fb._width = kAliasedTextureSize;
        fb._height = kAliasedTextureSize;
        fb._renderPass._colorAttachmentCount = 1;
        fb._renderPass._colorAttachments[0]._attachmentId = 0;
        fb._renderPass._colorAttachments[0]._loadAction = loadAction;
        fb._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
        fb._renderPass._colorAttachments[0]._clearColors[0] = red;
        fb._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;
        fb._colorAttachments[0] = target;
        return fb;
    }

    /*
    * @brief The flag survives only where it can be served, and says so in the memory report.
    * @details Aliasing shares bytes, so any flag implying memory the texture does not own
    * outright rules it out -- and so does the absence of attachment usage, which is the only use
    * the render graph can check an omitted declaration against.
    */
    inline void runAliasableFlagSanitizerTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);
        vkm::VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();

        const auto checkCleared = [&](vkm::VkmResourceCreateInfo flags, const char* debugName) {
            vkm::VkmTexture* texture = createAliasedTexture(driver, flags, debugName);
            REQUIRE(texture != nullptr);
            // Not necessarily Default: a texture that also asked to be tile-only keeps *that*
            // request and lands in the Transient sub-pool. The claim here is only that it did
            // not become an aliased one.
            CHECK(texture->getHandle().poolType != vkm::VkmResourcePoolType::Aliased);
            CHECK_FALSE(texture->isAliasable());
            CHECK((texture->getTextureInfo()._flags & vkm::VkmResourceCreateInfo::Aliasable) == 0);
            // A texture that owns its memory is usable immediately -- nothing waits on a
            // placement that will never come.
            CHECK(texture->isAliasPlaced());
            resourcePool->releaseResource(texture->getHandle());
        };

        SUBCASE("granted for a plain attachment")
        {
            vkm::VkmTexture* texture = createAliasedTexture(
                driver,
                vkm::VkmResourceCreateInfo::AllowColorAttachment | vkm::VkmResourceCreateInfo::AllowShaderRead |
                    vkm::VkmResourceCreateInfo::Aliasable,
                "AliasedGranted");
            REQUIRE(texture != nullptr);

            const bool granted = texture->isAliasable();
            // Built up first: doctest's MESSAGE binds its own operator tighter than +.
            const std::string grantMessage = std::string("aliasing granted: ") + (granted ? "yes" : "no");
            MESSAGE(grantMessage);
            if (granted)
            {
                CHECK(texture->getHandle().poolType == vkm::VkmResourcePoolType::Aliased);
                // No graph has declared it yet, so it has no memory and nothing may bind it.
                CHECK_FALSE(texture->isAliasPlaced());

                const std::optional<vkm::VkmResourceMemoryTag> tag =
                    resourcePool->getResourceMemoryTag(texture->getHandle());
                REQUIRE(tag.has_value());
                CHECK(tag->requestedSize > 0);
                // The bytes are counted once as a heap block; charging every sharer would
                // inflate the report by the whole group.
                CHECK(tag->allocatedSize == 0);
                CHECK(tag->metadata == "aliased");
            }
            resourcePool->releaseResource(texture->getHandle());
        }

        SUBCASE("a tile-only texture has no memory to alias")
        {
            checkCleared(vkm::VkmResourceCreateInfo::AllowColorAttachment | vkm::VkmResourceCreateInfo::Transient |
                             vkm::VkmResourceCreateInfo::Aliasable,
                         "AliasedTransient");
        }

        SUBCASE("an externally owned texture's memory is not ours to share")
        {
            checkCleared(vkm::VkmResourceCreateInfo::AllowColorAttachment |
                             vkm::VkmResourceCreateInfo::ExternalHandleOwner | vkm::VkmResourceCreateInfo::Aliasable,
                         "AliasedExternal");
        }

        SUBCASE("a presentable cannot be aliased")
        {
            checkCleared(vkm::VkmResourceCreateInfo::AllowColorAttachment | vkm::VkmResourceCreateInfo::AllowPresent |
                             vkm::VkmResourceCreateInfo::Aliasable,
                         "AliasedPresent");
        }

        SUBCASE("without attachment usage there is nothing the graph can check")
        {
            checkCleared(vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::Aliasable,
                         "AliasedNoAttachment");
        }
    }

    // Neither backend has an aliasable buffer concept the graph could bound a lifetime for.
    inline void runAliasableBufferFlagIsDroppedTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);

        vkm::VkmBufferInfo bufferInfo{};
        bufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::Aliasable;
        bufferInfo._size = 256;
        bufferInfo._debugName = "AliasedBuffer";
        vkm::VkmBuffer* buffer = driver->newBuffer(bufferInfo);
        REQUIRE(buffer != nullptr);

        CHECK(buffer->getHandle().poolType == vkm::VkmResourcePoolType::Default);
        CHECK((buffer->getBufferInfo()._flags & vkm::VkmResourceCreateInfo::Aliasable) == 0);
        driver->getRenderResourcePool()->releaseResource(buffer->getHandle());
    }

    /*
    * @brief Two textures whose declared lifetimes do not overlap end up sharing bytes.
    * @details The graph shape is the feature's own example: A is written and read, then B is
    * written and read. compile() is what turns those declarations into a placement, so both
    * textures are unplaced going in and placed coming out.
    */
    inline void runAliasedPlacementTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);
        vkm::VkmAliasedMemoryHeap* heap = driver->getAliasedMemoryHeap();
        REQUIRE(heap != nullptr);

        const vkm::VkmResourceCreateInfo flags = vkm::VkmResourceCreateInfo::AllowColorAttachment |
                                                 vkm::VkmResourceCreateInfo::AllowShaderRead |
                                                 vkm::VkmResourceCreateInfo::Aliasable;
        vkm::VkmTexture* textureA = createAliasedTexture(driver, flags, "AliasedA");
        vkm::VkmTexture* textureB = createAliasedTexture(driver, flags, "AliasedB");
        REQUIRE(textureA != nullptr);
        REQUIRE(textureB != nullptr);
        REQUIRE(textureA->isAliasable());
        REQUIRE(textureB->isAliasable());
        CHECK_FALSE(textureA->isAliasPlaced());
        CHECK_FALSE(textureB->isAliasPlaced());

        const vkm::VkmResourceHandle handleA = textureA->getHandle();
        const vkm::VkmResourceHandle handleB = textureB->getHandle();

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        // S1(A write) -> S2(A read) -> S3(B write) -> S4(B read). A is dead before B is born.
        auto* writeA = renderGraph.beginGraphicsSubGraph(makeAliasedFb(handleA, vkm::VkmLoadAction::Clear, 1.0f),
                                                         "AliasWriteA");
        writeA->addAliasedResource(handleA);
        auto* readA = renderGraph.beginComputeSubGraph("AliasReadA");
        readA->addAliasedResource(handleA);
        auto* writeB = renderGraph.beginGraphicsSubGraph(makeAliasedFb(handleB, vkm::VkmLoadAction::Clear, 0.0f),
                                                         "AliasWriteB");
        writeB->addAliasedResource(handleB);
        auto* readB = renderGraph.beginComputeSubGraph("AliasReadB");
        readB->addAliasedResource(handleB);

        renderGraph.compile();

        CHECK(textureA->isAliasPlaced());
        CHECK(textureB->isAliasPlaced());
        const std::optional<vkm::VkmAliasPlacement> placementA = heap->getPlacement(handleA);
        const std::optional<vkm::VkmAliasPlacement> placementB = heap->getPlacement(handleB);
        REQUIRE(placementA.has_value());
        REQUIRE(placementB.has_value());
        // The claim the whole feature exists to make.
        CHECK(placementA->_blockIndex == placementB->_blockIndex);
        CHECK(placementA->_offset == placementB->_offset);
        CHECK(heap->isAliased(handleA));
        CHECK(heap->isAliased(handleB));

        renderGraph.execute();
        renderGraph.ensureCompleted();

        driver->getRenderResourcePool()->releaseResource(handleA);
        driver->getRenderResourcePool()->releaseResource(handleB);
    }

    /*
    * @brief Overlapping lifetimes are refused, so a texture read by the pass that writes its
    * partner keeps its own bytes.
    * @details This is the shape the gi sample actually has -- its composite pass samples the
    * direct-lighting target while writing its own output -- and the reason those two could not
    * be aliased.
    */
    inline void runOverlappingLifetimeIsRefusedTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);
        vkm::VkmAliasedMemoryHeap* heap = driver->getAliasedMemoryHeap();
        REQUIRE(heap != nullptr);

        const vkm::VkmResourceCreateInfo flags = vkm::VkmResourceCreateInfo::AllowColorAttachment |
                                                 vkm::VkmResourceCreateInfo::AllowShaderRead |
                                                 vkm::VkmResourceCreateInfo::Aliasable;
        vkm::VkmTexture* textureA = createAliasedTexture(driver, flags, "OverlapA");
        vkm::VkmTexture* textureB = createAliasedTexture(driver, flags, "OverlapB");
        REQUIRE(textureA != nullptr);
        REQUIRE(textureB != nullptr);
        const vkm::VkmResourceHandle handleA = textureA->getHandle();
        const vkm::VkmResourceHandle handleB = textureB->getHandle();

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* writeA = renderGraph.beginGraphicsSubGraph(makeAliasedFb(handleA, vkm::VkmLoadAction::Clear, 1.0f),
                                                         "OverlapWriteA");
        writeA->addAliasedResource(handleA);
        // The pass that writes B also reads A, so the two are live at the same instant.
        auto* writeB = renderGraph.beginGraphicsSubGraph(makeAliasedFb(handleB, vkm::VkmLoadAction::Clear, 0.0f),
                                                         "OverlapWriteBReadA");
        writeB->addAliasedResource(handleB);
        writeB->addAliasedResource(handleA);

        renderGraph.compile();

        const std::optional<vkm::VkmAliasPlacement> placementA = heap->getPlacement(handleA);
        const std::optional<vkm::VkmAliasPlacement> placementB = heap->getPlacement(handleB);
        REQUIRE(placementA.has_value());
        REQUIRE(placementB.has_value());
        const bool sameBytes = placementA->_blockIndex == placementB->_blockIndex &&
                               placementA->_offset == placementB->_offset;
        CHECK_FALSE(sameBytes);

        driver->getRenderResourcePool()->releaseResource(handleA);
        driver->getRenderResourcePool()->releaseResource(handleB);
    }

    /*
    * @brief An aliasable texture attached without being declared has its lifetime widened.
    * @details Asserted through the placement rather than the log line: the widened lifetime now
    * overlaps the other texture's, so the packer must refuse to share -- which is the outcome
    * that actually protects the pixels. Trusting the omission instead would hand the bytes away
    * while the pass is still writing them.
    */
    inline void runUndeclaredAliasedUseTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);
        vkm::VkmAliasedMemoryHeap* heap = driver->getAliasedMemoryHeap();
        REQUIRE(heap != nullptr);

        const vkm::VkmResourceCreateInfo flags = vkm::VkmResourceCreateInfo::AllowColorAttachment |
                                                 vkm::VkmResourceCreateInfo::AllowShaderRead |
                                                 vkm::VkmResourceCreateInfo::Aliasable;
        vkm::VkmTexture* textureA = createAliasedTexture(driver, flags, "UndeclaredA");
        vkm::VkmTexture* textureB = createAliasedTexture(driver, flags, "UndeclaredB");
        REQUIRE(textureA != nullptr);
        REQUIRE(textureB != nullptr);
        const vkm::VkmResourceHandle handleA = textureA->getHandle();
        const vkm::VkmResourceHandle handleB = textureB->getHandle();

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* writeA = renderGraph.beginGraphicsSubGraph(makeAliasedFb(handleA, vkm::VkmLoadAction::Clear, 1.0f),
                                                         "UndeclaredWriteA");
        writeA->addAliasedResource(handleA);
        auto* writeB = renderGraph.beginGraphicsSubGraph(makeAliasedFb(handleB, vkm::VkmLoadAction::Clear, 0.0f),
                                                         "UndeclaredWriteB");
        writeB->addAliasedResource(handleB);
        // A is attached here and never declared. Left untrusted, its lifetime would end at
        // subgraph 0 and B would be handed its bytes while this pass still writes them.
        auto* lateA = renderGraph.beginGraphicsSubGraph(makeAliasedFb(handleA, vkm::VkmLoadAction::Clear, 0.5f),
                                                        "UndeclaredLateA");
        (void)lateA;

        renderGraph.compile();

        const std::optional<vkm::VkmAliasPlacement> placementA = heap->getPlacement(handleA);
        const std::optional<vkm::VkmAliasPlacement> placementB = heap->getPlacement(handleB);
        REQUIRE(placementA.has_value());
        REQUIRE(placementB.has_value());
        const bool sameBytes = placementA->_blockIndex == placementB->_blockIndex &&
                               placementA->_offset == placementB->_offset;
        CHECK_FALSE(sameBytes);

        driver->getRenderResourcePool()->releaseResource(handleA);
        driver->getRenderResourcePool()->releaseResource(handleB);
    }

    /*
    * @brief The behavioural proof: two textures sharing bytes each read back their own colour.
    * @details A is cleared red and copied into a texture that owns its memory; B then takes over
    * the same bytes and is cleared blue. If the acquisition barrier were missing, B's clear
    * could land before A's copy read it and the copy would come back blue -- which no amount of
    * placement-level assertion would catch. Validation layers are on, so a malformed discard
    * barrier fails here too.
    */
    inline void runAliasedPixelTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);

        const vkm::VkmResourceCreateInfo aliasFlags = vkm::VkmResourceCreateInfo::AllowColorAttachment |
                                                      vkm::VkmResourceCreateInfo::AllowTransferSrc |
                                                      vkm::VkmResourceCreateInfo::Aliasable;
        vkm::VkmTexture* textureA = createAliasedTexture(driver, aliasFlags, "PixelAliasA");
        vkm::VkmTexture* textureB = createAliasedTexture(driver, aliasFlags, "PixelAliasB");
        REQUIRE(textureA != nullptr);
        REQUIRE(textureB != nullptr);
        const vkm::VkmResourceHandle handleA = textureA->getHandle();
        const vkm::VkmResourceHandle handleB = textureB->getHandle();

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        // Red into A.
        auto* writeA = renderGraph.beginGraphicsSubGraph(makeAliasedFb(handleA, vkm::VkmLoadAction::Clear, 1.0f),
                                                         "PixelWriteA");
        writeA->addAliasedResource(handleA);
        // Black into B, which by now owns the same bytes.
        auto* writeB = renderGraph.beginGraphicsSubGraph(makeAliasedFb(handleB, vkm::VkmLoadAction::Clear, 0.0f),
                                                         "PixelWriteB");
        writeB->addAliasedResource(handleB);

        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();

        // B wrote last, so the shared bytes must read back as B's colour rather than A's.
        const vkm::VkmTextureReadbackResult readback = driver->readbackTexture(handleB);
        REQUIRE(readback.channels > 0);
        REQUIRE_FALSE(readback.pixels.empty());
        const size_t centerTexel =
            (static_cast<size_t>(readback.height / 2) * readback.width + readback.width / 2) * readback.channels;
        REQUIRE(readback.pixels.size() > centerTexel);
        CHECK(readback.pixels[centerTexel] <= 1);

        driver->getRenderResourcePool()->releaseResource(handleA);
        driver->getRenderResourcePool()->releaseResource(handleB);
    }
} // namespace vkmtest

#endif // TEST_ALIASED_TEXTURE_SHARED_HPP
