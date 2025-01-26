// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/swapchain.h>

@protocol CAMetalDrawable;

namespace vkm
{
    class VkmSwapChainMetal final : public VkmSwapChainBase
    {
    public:
        VkmSwapChainMetal(VkmDriverBase* driver);
        ~VkmSwapChainMetal();
        
        void overrideCurrentDrawable(id<CAMetalDrawable> currentDrawable);
        
        virtual void setDebugName(const char* name) override final;

    protected:
        virtual bool createSwapChain(void* windowHandle) override final;
        virtual void destroySwapChain() override final;
        virtual VkmResourceHandle acquireNextImageInner() override final;
        virtual void presentInner() override final;
        
    private:
        id<CAMetalDrawable> _currentDrawable = nullptr;
    };
} // namespace vkm
