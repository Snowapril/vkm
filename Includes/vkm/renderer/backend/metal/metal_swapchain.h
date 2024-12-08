// Copyright (c) 2024 Snowapril

#include "vkm/renderer/backend/common/swapchain.h"

namespace vkm
{
    class MetalSwapChain final : public SwapChain
    {
    public:
        MetalSwapChain();
        ~MetalSwapChain();

    protected:
        virtual bool createSwapChain(const char* title) override final;
        virtual void destroySwapChain() override final;
        virtual uint8_t acquireNextImageIndexInner() override final;
        virtual void presentInner() override final;

    };
} // namespace vkm