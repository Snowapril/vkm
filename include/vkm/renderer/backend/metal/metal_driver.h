// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/metal/metal_bindless_resource_manager.h>

#include <memory>
#include <string>
#include <vector>

@protocol MTLDevice;
@protocol MTLBuffer;
@protocol MTLCaptureScope;
namespace vkm
{
    class VkmGpuHeapPoolMetal;

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

        /*
        * @brief Suballocate a buffer from an existing (or newly grown) heap pool block.
        * Returns nil if allocation failed (e.g. size exceeds a single pool block).
        */
        id<MTLBuffer> allocateFromHeapPool(uint64_t sizeBytes, uint64_t alignment, uint64_t options);

        // Shadows VkmDriverBase::getBindlessResourceManager() with the Metal-typed manager
        // (the base member always holds a VkmBindlessResourceManagerMetal for this driver).
        inline VkmBindlessResourceManagerMetal* getBindlessResourceManager() const
        {
            return static_cast<VkmBindlessResourceManagerMetal*>(_bindlessResourceManager.get());
        }

#if defined(VKM_GPU_CAPTURE)
        // MTLCaptureScope begin/end + one-shot .gputrace capture -- see driver.h.
        virtual void onFrameBegin() override final;
        virtual void onFrameEnd() override final;
        virtual void requestGpuFrameCapture(uint32_t startFrameDelay, uint32_t frameCount) override final;
#endif // VKM_GPU_CAPTURE

    protected:
        virtual VkmInitResult initializeInner(const VkmEngineLaunchOptions* options) override final;
        virtual bool postInitializeInner() override final;
        virtual void destroyInner() override final;
        virtual VkmSwapChainBase* newSwapChainInner() override final;
        virtual VkmTexture* newTextureInner() override final;
        virtual VkmBuffer* newBufferInner() override final;
        virtual VkmStagingBuffer* newStagingBufferInner() override final;
        virtual VkmSampler* newSamplerInner() override final;
        virtual VkmTextureView* newTextureViewInner() override final;
        virtual VkmBufferView* newBufferViewInner() override final;
        virtual VkmCommandQueueBase* newCommandQueueInner() override final;
        virtual VkmPipelineStateBase* newPipelineStateInner() override final;
        virtual VkmRenderResourcePool* newRenderResourcePoolInner() override final;

    private:
        id<MTLDevice> _mtlDevice;
        std::vector<std::unique_ptr<VkmGpuHeapPoolMetal>> _heapPools;

#if defined(VKM_GPU_CAPTURE)
        // Frame-aligned capture scope on the Graphics MTL4 queue (created in
        // postInitializeInner when enableGpuCapture is set; owned +1 under MRC).
        id<MTLCaptureScope> _captureScope {nullptr};
        // Render-thread only: frames to skip before starting a requested capture, and
        // frames left to record (>0 while a capture is pending or active).
        uint32_t _captureStartCountdown {0};
        uint32_t _captureFramesRemaining {0};
        bool _programmaticCaptureActive {false}; // a startCaptureWithDescriptor is in flight
        std::string _pendingTracePath;
#endif // VKM_GPU_CAPTURE
    };
}