// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/texture.h>

class MTLTexture;

namespace vkm
{
    class VkmTextureMetal : public VkmTexture
    {
    public:
        VkmTextureMetal(VkmDriverBase* driver);
        ~VkmTextureMetal();

        static VkmTextureInfo getTextureInfoFromMTLTexture(MTLTexture* mtlTexture);

        virtual bool initialize(const VkmTextureInfo& info, void* externalHandleOrNull = nullptr) override final;
        virtual void setDebugName(const char* name) override final;
        
        inline MTLTexture* getInternalHandle() const { return _mtlTexture; }

    private:
        MTLTexture* _mtlTexture {nullptr};
    };
} // namespace vkm