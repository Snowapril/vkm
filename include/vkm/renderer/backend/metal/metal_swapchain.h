// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/swapchain.h>

class CAMetalDrawable;

namespace vkm
{
    class VkmSwapChainMetal final : public VkmSwapChainBase
    {
    public:
        VkmSwapChainMetal(VkmDriverBase* driver);
        ~VkmSwapChainMetal();
        
        void overrideCurrentDrawable(CAMetalDrawable* currentDrawable);
        
    protected:
        virtual bool createSwapChain(void* windowHandle) override final;
        virtual void destroySwapChain() override final;
        virtual VkmResourceHandle acquireNextImageInner() override final;
        virtual void presentInner() override final;
        
    private:
        CAMetalDrawable* _currentDrawable = nullptr;
    };
} // namespace vkm
