// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>

@protocol MTLDevice;
namespace vkm
{
    /*
    * @brief renderer backend driver base class
    * @details 
    */
    class VkmDriverMetal : public VkmDriverBase
    {
    public:
        VkmDriverMetal(id<MTLDevice> mtlDevice);
        ~VkmDriverMetal();

        /*
        * @brief Get driver metal device
        */
        inline id<MTLDevice> getMTLDevice() const { return _mtlDevice; }

    protected:
        virtual bool initializeInner(const VkmEngineLaunchOptions* options) override final;
        virtual void destroyInner() override final;
        virtual VkmSwapChainBase* newSwapChainInner() override final;
        virtual VkmTexture* newTextureInner() override final;
        virtual VkmCommandQueueBase* newCommandQueueInner() override final;

    private:
        id<MTLDevice> _mtlDevice;
    };
}