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
         * @brief Create texture with existing rhi external handle
         * @param externalHandle external handle of texture which is created or managed by api
         * @param info texture info
         */
        virtual VkmTexture* newTexture(void* externalHandle, const VkmTextureInfo& info) override final;

        /*
        * @brief Create swapchain with window info
        * @param windowInfo window info
        */
        virtual SwapChain* newSwapChain(const VkmWindowInfo& windowInfo) override final;

    protected:
        virtual bool initializeInner(const VkmEngineLaunchOptions* options) override final;
        virtual void destroyInner() override final;

    private:
        MTLDevice* _mtlDevice;
    };
}