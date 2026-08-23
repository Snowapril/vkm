// Copyright (c) 2026 Snowapril
#pragma once

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/texture.h>

#include <optional>
#include <string>

/*
* Sparse textures are the residency half of texture streaming: the same texture, the same view and
* the same bindless slot for its whole life, with mip levels gaining and losing memory underneath.
*
* Everything here is written as an implication rather than a value, because whether a device can do
* it is a property of the machine -- placement sparse is a hardware tier on Metal, a feature bit on
* Vulkan, and MoltenVK has neither. What is asserted is that the *request* is answered coherently:
* granted with a usable mip tail, or refused and downgraded to a fully backed texture that behaves
* exactly as it always did. A half-granted sparse texture is the failure this guards against.
*/
namespace vkmtest
{
    inline void runSparseTextureTest(vkm::VkmDriverBase* driver)
    {
        const bool deviceSupportsSparse =
            (driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::SparseResidency) != 0u;
        // Built up first: doctest's MESSAGE binds its own operator tighter than +, so an inline
        // concatenation would be parsed against the message builder instead.
        const std::string deviceMessage =
            std::string("SparseResidency on this device: ") + (deviceSupportsSparse ? "yes" : "no");
        MESSAGE(deviceMessage);

        vkm::VkmTextureInfo info{};
        info._flags = static_cast<vkm::VkmResourceCreateInfo>(
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead) |
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferDst) |
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::Sparse));
        // Wide enough to have a tail well above level 0: at a 16 KiB page the tile is 64x64, so a
        // 1024-wide chain keeps five levels streamable before everything else collapses into it.
        info._extent = glm::uvec3(1024, 1024, 1);
        info._numMipLevels = 11;
        info._numArrayLayers = 1;
        info._format = vkm::VkmFormat::R8G8B8A8_UNORM;
        info._debugName = "SparseTest";

        vkm::VkmTexture* texture = driver->newTexture(info);
        REQUIRE(texture != nullptr);
        const vkm::VkmResourceHandle handle = texture->getHandle();
        CHECK(handle.isValid());

        const bool granted = texture->isSparse();
        const std::string grantMessage = std::string("sparse granted: ") + (granted ? "yes" : "no");
        MESSAGE(grantMessage);
        // The request may be refused, but only where the device said it would be.
        CHECK((!granted || deviceSupportsSparse));
        // And a device that reports the capability must honour it, or the flag is a lie.
        CHECK((!deviceSupportsSparse || granted));

        if (granted)
        {
            /*
            * A tail at level 0 would mean the whole chain is one indivisible allocation -- the
            * texture would be nominally sparse and completely unstreamable, which is worse than an
            * honest refusal because nothing downstream would notice.
            */
            CHECK(texture->getMipTailFirstLevel() > 0);
            CHECK(texture->getMipTailFirstLevel() <= info._numMipLevels);

            // The bytes of a sparse texture live in the tile heap, not in the texture: its own
            // allocated size is page-table overhead that does not move as tiles come and go, so
            // reporting it would put a number in the memory report that never changes.
            const std::optional<vkm::VkmResourceMemoryTag> tag =
                driver->getRenderResourcePool()->getResourceMemoryTag(handle);
            REQUIRE(tag.has_value());
            CHECK(tag->allocatedSize == 0);
        }
        else
        {
            // Downgraded, not half-done: a refused request leaves an ordinary texture, and an
            // ordinary texture has no tail to speak of.
            CHECK(texture->getMipTailFirstLevel() == info._numMipLevels);
        }

        driver->getRenderResourcePool()->releaseResource(handle);
    }

    /*
    * @brief Sparse is a request like Transient and Aliasable, and the same combinations that decide
    * backing memory another way must win over it rather than producing a texture that is both.
    */
    inline void runSparseFlagSanitizeTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmTextureInfo info{};
        info._extent = glm::uvec3(256, 256, 1);
        info._numMipLevels = 9;
        info._numArrayLayers = 1;
        info._format = vkm::VkmFormat::R8G8B8A8_UNORM;

        SUBCASE("Sparse loses to Transient, which has no memory to page")
        {
            info._flags = static_cast<vkm::VkmResourceCreateInfo>(
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowColorAttachment) |
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::Transient) |
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::Sparse));
            info._debugName = "SparseAndTransient";

            vkm::VkmTexture* texture = driver->newTexture(info);
            REQUIRE(texture != nullptr);
            CHECK(!texture->isSparse());
            CHECK((texture->getTextureInfo()._flags & vkm::VkmResourceCreateInfo::Sparse) == 0);
            driver->getRenderResourcePool()->releaseResource(texture->getHandle());
        }

        SUBCASE("Sparse is texture-only and is dropped from a buffer")
        {
            vkm::VkmBufferInfo bufferInfo{};
            bufferInfo._flags = static_cast<vkm::VkmResourceCreateInfo>(
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead) |
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::Sparse));
            bufferInfo._size = 4096;
            bufferInfo._debugName = "SparseBuffer";

            vkm::VkmBuffer* buffer = driver->newBuffer(bufferInfo);
            REQUIRE(buffer != nullptr);
            CHECK((buffer->getBufferInfo()._flags & vkm::VkmResourceCreateInfo::Sparse) == 0);
            driver->getRenderResourcePool()->releaseResource(buffer->getHandle());
        }
    }
} // namespace vkmtest
