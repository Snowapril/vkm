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
<<<<<<< HEAD
        * @brief Writes tightly-packed pixels straight into this texture's memory, with no staging
        * buffer and no queue submission. Only call this when isHostWritable().
        * @details The counterpart of VkmCommandBufferBase::copyBufferToTexture, with the same
        * end-state contract: the texture is shader-readable once this returns.
        * @param data Source pixels, tightly packed.
        * @param size Number of bytes to write.
        * @param mipLevel Mip level to write.
        * @param arrayLayer Array layer to write.
        * @return False if the texture's memory cannot be written directly.
=======
        * @brief Whether this texture's contents live only in on-chip tile memory, with no
        * device memory backing at all.
        * @details Set by the backend at creation from what it actually allocated -- asking for
        * a transient texture is a request, not a guarantee, so this reports the outcome rather
        * than the intent. Metal: MTLStorageModeMemoryless, always granted. Vulkan: the
        * allocation reports LAZILY_ALLOCATED, which a device offering no such memory type will
        * not grant -- the image is then an ordinary device-local attachment. Always false on
        * WebGPU, which has no equivalent.
        *
        * True means the texture must never be sampled, blitted or read back, and that every
        * pass writing it must use VkmStoreAction::DontCare -- see
        * VkmResourceCreateInfo::Transient.
        */
        inline bool isTransient() const { return _isTransient; }

        /*
        * @brief Writes tightly-packed pixels straight into this texture's memory, with no
        * staging buffer and no queue submission. Only called when isHostWritable() is true.
        * @details The counterpart of VkmCommandBufferBase::copyBufferToTexture, and carries
        * the same end-state contract: the texture is shader-readable once this returns.
        * Not pure -- a backend that never reports isHostWritable() has nothing to implement,
        * and this default is the matching "should be unreachable" guard.
>>>>>>> e3e8755 (Add a Transient (tile-memory-only) resource pool type and create flag)
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
        // Safe default: a backend that cannot honor VkmResourceCreateInfo::Transient reports an
        // ordinary device-backed attachment, which is what it actually allocated.
        bool _isTransient = false;
    };
} // namespace vkm