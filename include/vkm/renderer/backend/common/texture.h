// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/aliased_memory_heap.h>
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
        * @brief Whether this texture's memory is shared with other textures whose render-graph
        * lifetimes do not overlap its own.
        * @details Set by the backend at creation from what it can actually do -- asking for an
        * aliased texture is a request, not a guarantee, so this reports the outcome rather than
        * the intent. False on WebGPU, which has no aliasing at all.
        *
        * True means the texture's contents are undefined at the start of every frame and that
        * every subgraph touching it must declare it -- see VkmResourceCreateInfo::Aliasable.
        */
        inline bool isAliasable() const { return _isAliasable; }

        /*
        * @brief Whether this texture's backing memory has been assigned yet.
        * @details An aliasable texture is created without memory: its placement is chosen by the
        * first VkmRenderGraph::compile() that declares it, and is final from then on. Until this
        * returns true the texture has no usable native handle and must not be attached, bound
        * into a VkmResourceTable, or registered bindless. Always true for a non-aliasable
        * texture, which owns its memory from creation.
        */
        inline bool isAliasPlaced() const { return !_isAliasable || _isAliasPlaced; }

        /*
        * @brief Whether which mip levels are backed can change without recreating this texture.
        * @details Set by the backend at creation from what it could actually do -- asking for a
        * sparse texture is a request, not a guarantee, so this reports the outcome. False wherever
        * the driver lacks VkmDriverCapabilityFlags::SparseResidency, where the texture is fully
        * backed instead and behaves exactly as it always did.
        *
        * True means levels outside the mip tail start unbacked and must be given memory before
        * they are uploaded or sampled -- see VkmResourceCreateInfo::Sparse.
        */
        inline bool isSparse() const { return _isSparse; }

        /*
        * @brief First mip level of the indivisible, permanently backed tail.
        * @details Every level from here down is too small to fill one tile and shares a single
        * allocation that is bound for the texture's life, so streaming can neither evict nor
        * partially back it. Equal to the level count on a non-sparse texture, which has no tail.
        */
        inline uint32_t getMipTailFirstLevel() const { return _mipTailFirstLevel; }

        /*
        * @brief Writes tightly-packed pixels straight into this texture's memory, with no
        * staging buffer and no queue submission. Only called when isHostWritable() is true.
        * @details The counterpart of VkmCommandBufferBase::copyBufferToTexture, and carries
        * the same end-state contract: the texture is shader-readable once this returns.
        * Not pure -- a backend that never reports isHostWritable() has nothing to implement,
        * and this default is the matching "should be unreachable" guard.
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

        /*
        * @brief Bind this texture to the bytes the render graph chose for it, once.
        * @details Called by VkmRenderGraph::compile() for a texture the aliasing heap has just
        * placed. Not pure: a backend that never reports isAliasable() never receives one, and
        * this default is the matching "should be unreachable" guard.
        */
        virtual bool finalizeAliasPlacement(const VkmAliasPlacement& placement)
        {
            (void)placement;
            VKM_DEBUG_ERROR("finalizeAliasPlacement is not implemented for this texture");
            return false;
        }

    protected:
        bool initializeTextureCommon(VkmResourceHandle handle, const VkmTextureInfo& info);

    protected:
        VkmTextureInfo _textureInfo;
        std::vector<VkmResourceHandle> _ownedViewHandles;
        bool _isHostWritable = false;
        // Safe default: a backend that cannot honor VkmResourceCreateInfo::Transient reports an
        // ordinary device-backed attachment, which is what it actually allocated.
        bool _isTransient = false;
        // Safe default: a backend that cannot alias owns its memory outright, so nothing has to
        // wait for a placement that will never come (see isAliasPlaced).
        bool _isAliasable = false;
        bool _isAliasPlaced = false;
        bool _isSparse = false;
        // Level count until a backend reports a real tail, so "no tail" is the safe default:
        // getMipTailFirstLevel() then names a level past the end and nothing is treated as pinned.
        uint32_t _mipTailFirstLevel = 0;
    };
} // namespace vkm