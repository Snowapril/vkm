// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>

class MTLDevice;

namespace vkm
{
    /*
    * @brief renderer backend driver base class
    * @details 
    */
    class VkmDriverMetal : public VkmDriverBase
    {
    public:
        VkmDriverMetal(MTLDevice* mtlDevice);
        ~VkmDriverMetal();

        /*
        * @brief Create swapchain with window info
        * @param windowInfo window info
        */
        virtual SwapChain* newSwapChain(const VkmWindowInfo& windowInfo) override final;

    protected:
        virtual bool initializeInner(const VkmEngineLaunchOptions* options) override final;
        virtual void destroyInner() override final;
        virtual VkmTexture* newTextureInner() override final;

    private:
        MTLDevice* _mtlDevice;
    };
}