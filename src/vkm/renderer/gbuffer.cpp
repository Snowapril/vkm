// Copyright (c) 2025 Snowapril

#include <vkm/renderer/gbuffer.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/texture.h>

#include <string>

namespace vkm
{
    namespace
    {
        const char* targetDebugName(VkmGBuffer::Target target)
        {
            switch (target)
            {
                case VkmGBuffer::Target::Normal:             return "Normal";
                case VkmGBuffer::Target::BaseColorRoughness: return "BaseColorRoughness";
                case VkmGBuffer::Target::MotionMetallic:     return "MotionMetallic";
                case VkmGBuffer::Target::Count:              break;
            }
            return "Unknown";
        }
    }

    VkmFormat VkmGBuffer::getTargetFormat(Target target)
    {
        switch (target)
        {
            // Signed, and beyond 8-bit precision: an octahedral normal pair needs both, and 8-bit
            // UNORM normals band visibly under the low-frequency lighting a GI pass produces.
            case Target::Normal:             return VkmFormat::R16G16B16A16_SFLOAT;
            // Base colour is authored in [0,1] and roughness likewise, so 8-bit UNORM is enough.
            case Target::BaseColorRoughness: return VkmFormat::R8G8B8A8_UNORM;
            // Motion vectors are signed and can exceed 1 in UV units under fast motion.
            case Target::MotionMetallic:     return VkmFormat::R16G16B16A16_SFLOAT;
            case Target::Count:              break;
        }
        return VkmFormat::Undefined;
    }

    VkmFormat VkmGBuffer::getDepthFormat()
    {
        // No stencil: nothing in the GI path uses one, and D32 is the widely supported depth-only
        // format across the three backends.
        return VkmFormat::D32_SFLOAT;
    }

    VkmGBuffer::~VkmGBuffer()
    {
        // Not destroy(): a driver-owning caller may already be gone. Leaking here would be a bug,
        // so this asserts rather than silently tidying up.
        VKM_ASSERT(_driver == nullptr, "VkmGBuffer destroyed without destroy() being called first");
    }

    bool VkmGBuffer::initialize(VkmDriverBase* driver, const glm::uvec2& extent)
    {
        VKM_ASSERT(driver != nullptr, "VkmGBuffer needs a driver");
        if (extent.x == 0 || extent.y == 0)
        {
            VKM_DEBUG_ERROR("VkmGBuffer cannot be created at a zero extent");
            return false;
        }

        _driver = driver;
        _extent = extent;
        _currentSet = 0;

        for (uint32_t setIndex = 0; setIndex < _sets.size(); ++setIndex)
        {
            if (!createSet(_sets[setIndex], extent, setIndex))
            {
                destroy();
                return false;
            }
        }
        return true;
    }

    void VkmGBuffer::destroy()
    {
        if (_driver == nullptr)
        {
            return;
        }
        for (TextureSet& set : _sets)
        {
            releaseSet(set, /*deferred=*/false);
        }
        _driver = nullptr;
        _extent = glm::uvec2(0, 0);
        _currentSet = 0;
    }

    bool VkmGBuffer::resize(const glm::uvec2& extent)
    {
        if (_driver == nullptr)
        {
            VKM_DEBUG_ERROR("VkmGBuffer::resize before initialize");
            return false;
        }
        if (extent == _extent)
        {
            return true;
        }
        if (extent.x == 0 || extent.y == 0)
        {
            VKM_DEBUG_ERROR("VkmGBuffer cannot be resized to a zero extent");
            return false;
        }

        // Deferred: frames still in flight may be sampling the outgoing textures.
        for (TextureSet& set : _sets)
        {
            releaseSet(set, /*deferred=*/true);
        }

        _extent = extent;
        _currentSet = 0;
        for (uint32_t setIndex = 0; setIndex < _sets.size(); ++setIndex)
        {
            if (!createSet(_sets[setIndex], extent, setIndex))
            {
                return false;
            }
        }
        return true;
    }

    void VkmGBuffer::advanceFrame()
    {
        _currentSet = 1 - _currentSet;
    }

    VkmResourceHandle VkmGBuffer::getTexture(Target target) const
    {
        return _sets[_currentSet]._targets[static_cast<uint32_t>(target)];
    }

    VkmResourceHandle VkmGBuffer::getDepthTexture() const
    {
        return _sets[_currentSet]._depth;
    }

    VkmResourceHandle VkmGBuffer::getPrevTexture(Target target) const
    {
        return _sets[1 - _currentSet]._targets[static_cast<uint32_t>(target)];
    }

    VkmResourceHandle VkmGBuffer::getPrevDepthTexture() const
    {
        return _sets[1 - _currentSet]._depth;
    }

    VkmFrameBufferDescriptor VkmGBuffer::makeFrameBufferDescriptor() const
    {
        VkmFrameBufferDescriptor descriptor{};
        descriptor._width = _extent.x;
        descriptor._height = _extent.y;
        descriptor._renderPass._colorAttachmentCount = kTargetCount;
        for (uint32_t i = 0; i < kTargetCount; ++i)
        {
            VkmColorAttachmentDescriptor& attachment = descriptor._renderPass._colorAttachments[i];
            attachment._attachmentId = i;
            attachment._loadAction = VkmLoadAction::Clear;
            attachment._storeAction = VkmStoreAction::Store;
            attachment._clearColors[0] = 0.0f;
            attachment._clearColors[1] = 0.0f;
            attachment._clearColors[2] = 0.0f;
            attachment._clearColors[3] = 0.0f;
            descriptor._colorAttachments[i] = _sets[_currentSet]._targets[i];
        }

        VkmDepthStencilAttachmentDescriptor depthAttachment{};
        depthAttachment._attachmentId = kTargetCount;
        depthAttachment._loadAction = VkmLoadAction::Clear;
        depthAttachment._storeAction = VkmStoreAction::Store;
        // Reversed-Z is not in use; the engine's projection is [0,1] with 1 = far.
        depthAttachment._clearDepth = 1.0f;
        depthAttachment._clearStencil = 0;
        descriptor._renderPass._depthStencilAttachment = depthAttachment;
        descriptor._depthStencilAttachment = _sets[_currentSet]._depth;

        return descriptor;
    }

    bool VkmGBuffer::createSet(TextureSet& set, const glm::uvec2& extent, uint32_t setIndex)
    {
        for (uint32_t i = 0; i < kTargetCount; ++i)
        {
            const Target target = static_cast<Target>(i);
            const std::string debugName =
                std::string("VkmGBuffer") + targetDebugName(target) + std::to_string(setIndex);

            VkmTextureInfo info{};
            // Written as an attachment, then sampled by the lighting/GI passes -- which is what
            // barrierTextureForShaderRead() exists to hand off.
            info._flags = static_cast<VkmResourceCreateInfo>(
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowColorAttachment) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead));
            info._extent = glm::uvec3(extent, 1);
            info._numMipLevels = 1;
            info._numArrayLayers = 1;
            info._format = getTargetFormat(target);
            info._debugName = debugName.c_str();

            VkmTexture* texture = _driver->newTexture(info);
            if (texture == nullptr)
            {
                VKM_DEBUG_ERROR("Failed to create a G-buffer target");
                return false;
            }
            set._targets[i] = texture->getHandle();
        }

        const std::string depthDebugName = std::string("VkmGBufferDepth") + std::to_string(setIndex);
        VkmTextureInfo depthInfo{};
        depthInfo._flags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowDepthStencilAttachment) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead));
        depthInfo._extent = glm::uvec3(extent, 1);
        depthInfo._numMipLevels = 1;
        depthInfo._numArrayLayers = 1;
        depthInfo._format = getDepthFormat();
        depthInfo._debugName = depthDebugName.c_str();

        VkmTexture* depthTexture = _driver->newTexture(depthInfo);
        if (depthTexture == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create the G-buffer depth target");
            return false;
        }
        set._depth = depthTexture->getHandle();
        return true;
    }

    void VkmGBuffer::releaseSet(TextureSet& set, bool deferred)
    {
        const auto release = [this, deferred](VkmResourceHandle& handle) {
            if (!handle.isValid())
            {
                return;
            }
            if (deferred)
            {
                _driver->getDeferredReclaimer()->requestRelease(handle);
            }
            else
            {
                _driver->getRenderResourcePool()->releaseResource(handle);
            }
            handle = VKM_INVALID_RESOURCE_HANDLE;
        };

        for (VkmResourceHandle& handle : set._targets)
        {
            release(handle);
        }
        release(set._depth);
    }
} // namespace vkm
