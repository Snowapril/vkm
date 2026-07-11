// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/texture_view.h>

@protocol MTLTexture;

namespace vkm
{
    class VkmTextureViewMetal : public VkmTextureView
    {
    public:
        VkmTextureViewMetal(VkmDriverBase* driver);
        ~VkmTextureViewMetal();

        virtual bool initialize(VkmResourceHandle handle, const VkmTextureViewInfo& info) override final;
        virtual void setDebugName(const char* name) override final;

        inline id<MTLTexture> getTextureView() const { return _mtlTextureView; }

    private:
        id<MTLTexture> _mtlTextureView{nullptr};
    };
} // namespace vkm
