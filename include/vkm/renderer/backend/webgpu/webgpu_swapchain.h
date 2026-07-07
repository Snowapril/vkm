// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/swapchain.h>
#include <webgpu/webgpu.h>

namespace vkm
{
    class VkmSwapChainWebGPU final : public VkmSwapChainBase
    {
    public:
        VkmSwapChainWebGPU(VkmDriverBase* driver);
        ~VkmSwapChainWebGPU();

        virtual void setDebugName(const char* name) override final;

        inline WGPUTextureFormat getSurfaceFormat() const { return _surfaceFormat; }

    protected:
        virtual bool createSwapChain(void* windowHandle) override final;
        virtual void destroySwapChain() override final;
        virtual VkmResourceHandle acquireNextImageInner() override final;
        virtual void presentInner() override final;

    private:
        WGPUSurface _surface{nullptr};
        WGPUTextureFormat _surfaceFormat{WGPUTextureFormat_Undefined};
    };
} // namespace vkm
