// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/swapchain.h>

@protocol CAMetalDrawable;

namespace vkm
{
    class VkmTextureMetal;
    class VkmSwapChainMetal final : public VkmSwapChainBase
    {
    public:
        VkmSwapChainMetal(VkmDriverBase* driver);
        ~VkmSwapChainMetal();
        
        void overrideCurrentDrawable(id<CAMetalDrawable> currentDrawable);

        virtual void setDebugName(const char* name) override final;

        // Matches the CAMetalLayer.pixelFormat set up in platform/apple/application.mm
        // (MTLPixelFormatRGBA16Float), independent of the approximate VkmFormat stored on the
        // per-backbuffer VkmTextureMetal (which mirrors Vulkan/WebGPU's more limited format enum).
        inline VkmFormat getBackBufferFormat() const { return VkmFormat::R16G16B16A16_SFLOAT; }

    protected:
        virtual bool createSwapChain(void* windowHandle) override final;
        virtual void destroySwapChain() override final;
        virtual VkmResourceHandle acquireNextImageInner() override final;
        virtual void presentInner() override final;
        
    private:
        id<CAMetalDrawable> _currentDrawable = nullptr;
    };
} // namespace vkm
