// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/platform/common/window.h>
#include <vkm/renderer/backend/common/backend_util.h>

#include <array>
#include <vector>

namespace vkm
{
    struct VkmEngineLaunchOptions;
    class VkmTexture;
    class VkmSwapChainBase;
    class VkmCommandQueueBase;

    /*
    * @brief renderer backend driver base class
    * @details manage whole engine lifecycle and drive the render driver and other modules
    */
    class VkmDriverBase
    {
    public:
        VkmDriverBase();
        ~VkmDriverBase();

        bool initialize(const VkmEngineLaunchOptions* options);
        void destroy();

        /*
         * @brief Create texture with existing rhi external handle
         * @param info texture info
         */
        VkmTexture* newTexture(const VkmTextureInfo& info);

        /*
        * @brief Create swapchain with window info
        */
        VkmSwapChainBase* newSwapChain();
        
    protected:
        virtual bool initializeInner(const VkmEngineLaunchOptions* options) = 0;
        virtual void destroyInner() = 0;
        virtual VkmTexture* newTextureInner() = 0;
        virtual VkmSwapChainBase* newSwapChainInner() = 0;

    protected:
        std::array<std::vector<VkmCommandQueueBase*>, (uint8_t)VkmCommandQueueType::Count> _commandQueues;

    };
}