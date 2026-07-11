// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/texture.h>

@protocol MTLTexture;

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
        virtual void setDebugName(const char* name) override final;

        // Metal exposes no allocation-introspection API -- best-effort passthrough of the
        // requested size; 256 matches this backend's own MTLHeap pool alignment convention
        // (see metal_buffer.mm's allocateFromHeapPool call).
        uint64_t getAllocatedSize() const override { return computeTextureByteSize(_textureInfo); }
        uint32_t getMemoryAlignment() const override { return 256; }

        inline id<MTLTexture> getInternalHandle() const { return _mtlTexture; }

    private:
        id<MTLTexture> _mtlTexture {nullptr};
    };
} // namespace vkm