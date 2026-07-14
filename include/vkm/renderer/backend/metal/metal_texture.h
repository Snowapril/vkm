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

        uint64_t getAllocatedSize() const override { return _allocatedSize; }
        uint32_t getMemoryAlignment() const override { return _memoryAlignment; }

        inline id<MTLTexture> getInternalHandle() const { return _mtlTexture; }

    private:
        id<MTLTexture> _mtlTexture {nullptr};
        uint64_t _allocatedSize{0};
        uint32_t _memoryAlignment{256}; // sane default; overwritten with a real value at creation time
    };
} // namespace vkm