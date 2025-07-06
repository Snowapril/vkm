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
        
        inline id<MTLTexture> getInternalHandle() const { return _mtlTexture; }

    private:
        id<MTLTexture> _mtlTexture {nullptr};
    };
} // namespace vkm