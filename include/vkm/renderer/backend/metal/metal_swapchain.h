// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/swapchain.h>

@protocol CAMetalDrawable;
@protocol MTLResidencySet;

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

        // The engine-chosen swapchain color format (see VkmDriverBase::getSwapChainColorFormat),
        // which is also what the CAMetalLayer and the per-backbuffer VkmTextureMetal are created with.
        VkmFormat getBackBufferFormat() const;

    protected:
        virtual bool createSwapChain(void* windowHandle) override final;
        virtual void destroySwapChain() override final;
        virtual VkmResourceHandle acquireNextImageInner() override final;
        virtual void presentInner() override final;
        
    private:
        id<CAMetalDrawable> _currentDrawable = nullptr;
        // This swapchain's own CAMetalLayer.residencySet, attached to the present queue in
        // createSwapChain and detached in destroySwapChain. Held per-swapchain (not in a shared
        // single slot) so multiple windows each add/remove exactly their own set.
        id<MTLResidencySet> _layerResidencySet = nullptr;
    };
} // namespace vkm
