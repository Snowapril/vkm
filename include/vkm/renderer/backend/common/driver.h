// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/platform/common/window.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <array>
#include <vector>

namespace vkm
{
    enum class VkmDriverCapabilityFlags : uint32_t
    {
        None                    = 0x000000000,
        CommandBufferReusable   = 0x00000001,
    };

    inline VkmDriverCapabilityFlags operator|(VkmDriverCapabilityFlags lhs, VkmDriverCapabilityFlags rhs)
    {
        return static_cast<VkmDriverCapabilityFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    inline uint32_t operator&(VkmDriverCapabilityFlags lhs, VkmDriverCapabilityFlags rhs)
    {
        return static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs);
    }
    
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

        /*
        * @brief initialize each graphics api setup and create necessary resources
        */
        bool initialize(const VkmEngineLaunchOptions* options);

        /*
        * @brief create command queues for each necessary needs
        */
        bool setUpPredefinedCommandQueues();

        /*
        * @brief desrtroy all resources and clean up
        */
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

        /*
        * @brief Allocate command buffer from pool for given queue type and start command buffer scope with name
        * @param queueType command queue type
        * @param commandQueueIndex command queue index within given command queue type
        * @param name command buffer name
        */
        VkmCommandBufferBase* beginCommandBuffer(const VkmCommandQueueType queueType, const uint32_t commandQueueIndex, const char* name);

        inline VkmDriverCapabilityFlags getDriverCapabilityFlags() const { return _driverCapabilityFlags; }

    protected:
        VkmCommandQueueBase* newCommandQueue(const VkmCommandQueueType queueType, const uint32_t commandQueueIndex, const char* name);

    protected:
        virtual bool initializeInner(const VkmEngineLaunchOptions* options) = 0;
        virtual void destroyInner() = 0;
        virtual VkmTexture* newTextureInner() = 0;
        virtual VkmSwapChainBase* newSwapChainInner() = 0;
        virtual VkmCommandQueueBase* newCommandQueueInner() = 0;

    protected:
        std::array<std::vector<VkmCommandQueueBase*>, (uint8_t)VkmCommandQueueType::Count> _commandQueues;
        VkmDriverCapabilityFlags _driverCapabilityFlags;
    };
}