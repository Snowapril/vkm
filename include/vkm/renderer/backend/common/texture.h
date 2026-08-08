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
        * @details Reports the outcome, not the request: set by the backend at creation from what it
        * actually allocated. Metal, MTLStorageModeShared; Vulkan, the allocation reports
        * HOST_VISIBLE and the image carries VK_IMAGE_USAGE_HOST_TRANSFER_BIT. False everywhere
        * else, including every backend without VkmDriverCapabilityFlags::TextureUpload.
        */
        inline bool isHostWritable() const { return _isHostWritable; }

        /*
        * @brief Writes tightly-packed pixels straight into this texture's memory, with no staging
        * buffer and no queue submission. Only call this when isHostWritable().
        * @details The counterpart of VkmCommandBufferBase::copyBufferToTexture, with the same
        * end-state contract: the texture is shader-readable once this returns.
        * @param data Source pixels, tightly packed.
        * @param size Number of bytes to write.
        * @param mipLevel Mip level to write.
        * @param arrayLayer Array layer to write.
        * @return False if the texture's memory cannot be written directly.
        */
        virtual bool writeRegion(const void* data, uint64_t size, uint32_t mipLevel, uint32_t arrayLayer)
        {
            (void)data; (void)size; (void)mipLevel; (void)arrayLayer;
            VKM_DEBUG_ERROR("writeRegion is not implemented for this texture");
            return false;
        }

        /*
        * @brief Creates a view of this texture. The only supported way to obtain a VkmTextureView.
        * @param info View description. Its _texture field is always overwritten with this texture's
        * own handle, whatever the caller passed in.
        * @return The new view, owned by this texture.
        */
        VkmTextureView* createView(VkmTextureViewInfo info);

        std::vector<VkmResourceHandle> getOwnedChildHandles() const override { return _ownedViewHandles; }

    protected:
        bool initializeTextureCommon(VkmResourceHandle handle, const VkmTextureInfo& info);

    protected:
        VkmTextureInfo _textureInfo;
        std::vector<VkmResourceHandle> _ownedViewHandles;
        bool _isHostWritable = false;
    };
} // namespace vkm