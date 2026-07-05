// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/texture.h>
#include <webgpu/webgpu.h>

namespace vkm
{
    class VkmTextureWebGPU final : public VkmTexture
    {
    public:
        VkmTextureWebGPU(VkmDriverBase* driver);
        ~VkmTextureWebGPU();

        virtual bool initialize(VkmResourceHandle handle, const VkmTextureInfo& info) override final;
        virtual bool overrideExternalHandle(void* externalHandle) override final;
        virtual void setDebugName(const char* name) override final;

        inline WGPUTexture getWGPUTexture() const { return _wgpuTexture; }

    private:
        WGPUTexture _wgpuTexture{nullptr};
        bool _externallyOwned{false}; // true for swapchain-provided textures; skip destroy on release
    };
} // namespace vkm
