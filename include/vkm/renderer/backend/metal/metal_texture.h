// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/metal/metal_gpu_heap_allocator.h>

@protocol MTLTexture;
@class MTLTextureDescriptor;

namespace vkm
{
    class VkmTextureMetal : public VkmTexture
    {
    public:
        VkmTextureMetal(VkmDriverBase* driver);
        ~VkmTextureMetal();

        static VkmTextureInfo getTextureInfoFromMTLTexture(id<MTLTexture> mtlTexture);

        virtual bool initialize(VkmResourceHandle handle, const VkmTextureInfo& info) override final;
        virtual bool overrideExternalHandle(void* externalHandle) override final;
        virtual bool finalizeAliasPlacement(const VkmAliasPlacement& placement) override final;
        virtual bool writeRegion(const void* data, uint64_t size, uint32_t mipLevel, uint32_t arrayLayer) override final;
        virtual void setDebugName(const char* name) override final;

        uint64_t getAllocatedSize() const override { return _allocatedSize; }
        uint32_t getMemoryAlignment() const override { return _memoryAlignment; }

        inline id<MTLTexture> getInternalHandle() const { return _mtlTexture; }

    private:
        id<MTLTexture> _mtlTexture {nullptr};
        // Valid only on the heap-placed path; a placement heap reclaims nothing on its own, so
        // the destructor must hand this range back.
        VkmGpuHeapAllocatorMetal::Placement _heapPlacement{};
        // Held only between initialize() and finalizeAliasPlacement() for an aliasable texture:
        // Metal creates the object and places it in one call, so the descriptor has to survive
        // until the render graph has chosen an offset. Released as soon as it is consumed.
        MTLTextureDescriptor* _aliasDescriptor {nullptr};
        uint64_t _allocatedSize{0};
        uint32_t _memoryAlignment{256}; // sane default; overwritten with a real value at creation time
    };
} // namespace vkm