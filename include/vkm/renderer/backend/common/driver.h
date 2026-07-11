// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/platform/common/window.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <array>
#include <vector>
#include <memory>
#include <string>

namespace vkm
{
    struct VkmEngineLaunchOptions;
    struct VkmPipelineStateDescriptor;
    class VkmTexture;
    class VkmBuffer;
    class VkmStagingBuffer;
    class VkmSampler;
    class VkmTextureView;
    class VkmBufferView;
    class VkmSwapChainBase;
    class VkmCommandQueueBase;
    class VkmCommandDispatcher;
    class VkmRenderResourcePool;
    class VkmPipelineStateBase;
    class VkmDeferredResourceReclaimer;

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

    enum class VkmInitResultCode
    {
        Success,
        HardwareUnsupported,  // environment/hardware cannot satisfy this backend's requirements
        Failed,               // any other initialization error - a real bug, must not be skipped
    };

    struct VkmInitResult
    {
        VkmInitResultCode code = VkmInitResultCode::Success;
        std::string       reason;
    };

    /*
    * @brief renderer backend driver base class
    * @details manage whole engine lifecycle and drive the render driver and other modules
    */
    class VkmDriverBase
    {
    public:
        VkmDriverBase();
        virtual ~VkmDriverBase();

        /*
        * @brief initialize each graphics api setup and create necessary resources
        */
        VkmInitResult initialize(const VkmEngineLaunchOptions* options);

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
         * @brief Create buffer with the given buffer info
         */
        VkmBuffer* newBuffer(const VkmBufferInfo& info);

        /*
         * @brief Create staging buffer with the given staging buffer info
         */
        VkmStagingBuffer* newStagingBuffer(const VkmStagingBufferInfo& info);

        /*
         * @brief Create sampler with the given sampler info
         */
        VkmSampler* newSampler(const VkmSamplerInfo& info);

        /*
        * @brief Create swapchain with window info
        */
        VkmSwapChainBase* newSwapChain();

        /*
        * @brief Create a backend pipeline state object from a fully-resolved descriptor
        * @param desc fully-resolved pipeline state descriptor (already option-expanded)
        * @param shaderCacheDir directory containing this descriptor's .vfcache files
        */
        VkmPipelineStateBase* newPipelineState(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError = nullptr);

        /*
        * @brief get driver capability flags
        */
        inline VkmDriverCapabilityFlags getDriverCapabilityFlags() const { return _driverCapabilityFlags; }

        // @brief get command dispatcher for the driver
        inline VkmCommandQueueBase* getCommandQueue(const VkmCommandQueueType queueType, const uint32_t commandQueueIndex) const
        {
            VKM_ASSERT(queueType < VkmCommandQueueType::Count, "Invalid command queue type");
            VKM_ASSERT(commandQueueIndex < _commandQueues[(uint8_t)queueType].size(), "Invalid command queue index");
            return _commandQueues[(uint8_t)queueType][commandQueueIndex];
        }

        /*
        * @brief get render resource pool
        */
        inline VkmRenderResourcePool* getRenderResourcePool() const { return _renderResourcePool.get(); }

        /*
        * @brief get deferred resource reclaimer; VkmRenderGraph drives its per-frame
        * pollOnce() fallback on WASM through this accessor.
        */
        inline VkmDeferredResourceReclaimer* getDeferredReclaimer() const { return _deferredReclaimer.get(); }

    protected:
        VkmCommandQueueBase* newCommandQueue(const VkmCommandQueueType queueType, const uint32_t commandQueueIndex, const char* name);

        /*
         * @brief Create a texture view referencing an existing (pooled) texture. Protected
         * and friended to VkmTexture only -- views must be created via
         * VkmTexture::createView() so ownership is tracked; nothing else may call this.
         */
        VkmTextureView* newTextureView(const VkmTextureViewInfo& info);

        /*
         * @brief Create a buffer view referencing an existing (pooled) buffer. Protected
         * and friended to VkmBuffer only -- views must be created via
         * VkmBuffer::createView() so ownership is tracked; nothing else may call this.
         */
        VkmBufferView* newBufferView(const VkmBufferViewInfo& info);

        friend class VkmTexture;
        friend class VkmBuffer;

    protected:
        virtual VkmInitResult initializeInner(const VkmEngineLaunchOptions* options) = 0;
        virtual void destroyInner() = 0;
        virtual VkmTexture* newTextureInner() = 0;
        virtual VkmBuffer* newBufferInner() = 0;
        virtual VkmStagingBuffer* newStagingBufferInner() = 0;
        virtual VkmSampler* newSamplerInner() = 0;
        virtual VkmTextureView* newTextureViewInner() = 0;
        virtual VkmBufferView* newBufferViewInner() = 0;
        virtual VkmSwapChainBase* newSwapChainInner() = 0;
        virtual VkmCommandQueueBase* newCommandQueueInner() = 0;
        virtual VkmPipelineStateBase* newPipelineStateInner() = 0;

    protected:
        std::array<std::vector<VkmCommandQueueBase*>, (uint8_t)VkmCommandQueueType::Count> _commandQueues;
        VkmDriverCapabilityFlags _driverCapabilityFlags;

    private:
        std::unique_ptr<VkmRenderResourcePool> _renderResourcePool;
        std::unique_ptr<VkmDeferredResourceReclaimer> _deferredReclaimer;
    };
}