// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource.h>

#include <vector>

namespace vkm
{
    class VkmTextureView;

    class VkmTexture : public VkmRenderResource
    {
    public:
        VkmTexture(VkmDriverBase* driver);
        ~VkmTexture();

        virtual bool initialize(VkmResourceHandle handle, const VkmTextureInfo& info) = 0;
        virtual bool overrideExternalHandle(void* externalHandle) = 0;

        inline const VkmTextureInfo& getTextureInfo() const { return _textureInfo; }
        VkmResourceType getResourceType() const override { return VkmResourceType::Texture; }

        /*
        * @brief Whether this texture's memory can be written by the CPU directly, as opposed
        * to through a staging buffer and a queue-submitted copy.
        * @details Set by the backend at creation from what it actually allocated -- asking
        * for host-writable memory is a request, not a guarantee, so this reports the outcome
        * rather than the intent. Metal: MTLStorageModeShared. Vulkan: the allocation reports
        * HOST_VISIBLE and the image carries VK_IMAGE_USAGE_HOST_TRANSFER_BIT. False
        * everywhere else, including every backend without
        * VkmDriverCapabilityFlags::TextureUpload, which keeps the staging path the default.
        */
        inline bool isHostWritable() const { return _isHostWritable; }

        /*
        * @brief Writes tightly-packed pixels straight into this texture's memory, with no
        * staging buffer and no queue submission. Only called when isHostWritable() is true.
        * @details The counterpart of VkmCommandBufferBase::copyBufferToTexture, and carries
        * the same end-state contract: the texture is shader-readable once this returns.
        * Not pure -- a backend that never reports isHostWritable() has nothing to implement,
        * and this default is the matching "should be unreachable" guard.
        */
        virtual bool writeRegion(const void* data, uint64_t size, uint32_t mipLevel, uint32_t arrayLayer)
        {
            (void)data; (void)size; (void)mipLevel; (void)arrayLayer;
            VKM_DEBUG_ERROR("writeRegion is not implemented for this texture");
            return false;
        }

        /*
        * @brief Create a view of this texture. This is the ONLY way to create a
        * VkmTextureView -- VkmDriverBase::newTextureView() is protected and friended to
        * this class specifically so callers cannot bypass ownership tracking. info._texture
        * is always overwritten with this texture's own handle, regardless of what the
        * caller passed in.
        */
        VkmTextureView* createView(VkmTextureViewInfo info);

        std::vector<VkmResourceHandle> getOwnedChildHandles() const override { return _ownedViewHandles; }

    protected:
        bool initializeTextureCommon(VkmResourceHandle handle, const VkmTextureInfo& info);

    protected:
        VkmTextureInfo _textureInfo;
        std::vector<VkmResourceHandle> _ownedViewHandles;
        // Safe default: a backend that never opts in keeps the staging upload path.
        bool _isHostWritable = false;
    };
} // namespace vkm