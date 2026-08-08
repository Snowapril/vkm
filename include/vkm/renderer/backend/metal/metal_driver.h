// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/metal/metal_bindless_resource_manager.h>
#include <vkm/renderer/backend/metal/metal_frame_constant_manager.h>

#include <memory>
#include <string>
#include <vector>

@protocol MTLDevice;
@protocol MTLBuffer;
@protocol MTLCaptureScope;
@protocol MTL4CounterHeap;
namespace vkm
{
    class VkmGpuHeapPoolMetal;

    /*
    * @brief Metal renderer backend driver.
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
        * @brief Suballocate a buffer from an existing, or newly grown, heap pool block.
        * @param sizeBytes Size to allocate.
        * @param alignment Required alignment.
        * @param options MTLResourceOptions for the allocation.
        * @return The buffer, or nil when the allocation failed, e.g. the size exceeds one block.
        */
        id<MTLBuffer> allocateFromHeapPool(uint64_t sizeBytes, uint64_t alignment, uint64_t options);

        // Device-reported allocation size/budget plus the heap pool's reserved-vs-used split.
        virtual VkmGpuMemoryStats getGpuMemoryStats() const override final;

        // GPU timestamp pool backing VkmGpuProfiler: one MTL4CounterHeap of `slotCount`
        // MTL4CounterHeapTypeTimestamp entries. See driver.h for the contract.
        virtual bool initializeGpuTimestampPool(uint32_t slotCount) override final;
        virtual void destroyGpuTimestampPool() override final;
        virtual double getGpuTimestampPeriodNs() const override final { return _timestampPeriodNs; }
        virtual void resetGpuTimestampSlots(VkmCommandBufferBase* commandBuffer, uint32_t firstSlot,
                                            uint32_t count) override final;
        virtual bool resolveGpuTimestamps(uint32_t firstSlot, uint32_t count, uint64_t* outTicks) override final;

        inline id<MTL4CounterHeap> getGpuTimestampCounterHeap() const { return _timestampCounterHeap; }

        // Shadows VkmDriverBase::getBindlessResourceManager() with the Metal-typed manager
        // (the base member always holds a VkmBindlessResourceManagerMetal for this driver).
        inline VkmBindlessResourceManagerMetal* getBindlessResourceManager() const
        {
            return static_cast<VkmBindlessResourceManagerMetal*>(_bindlessResourceManager.get());
        }

        // Shadows VkmDriverBase::getFrameConstantManager() with the Metal-typed manager, for
        // the same reason as above.
        inline VkmFrameConstantManagerMetal* getFrameConstantManager() const
        {
            return static_cast<VkmFrameConstantManagerMetal*>(_frameConstantManager.get());
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
        virtual VkmResourceTableBase* newResourceTableInner() override final;
        virtual VkmAccelerationStructure* newAccelerationStructureInner() override final;
        virtual VkmTexture* newTextureInner() override final;
        virtual VkmBuffer* newBufferInner() override final;
        virtual VkmStagingBuffer* newStagingBufferInner() override final;
        virtual VkmSampler* newSamplerInner() override final;
        virtual VkmTextureView* newTextureViewInner() override final;
        virtual VkmBufferView* newBufferViewInner() override final;
        virtual VkmCommandQueueBase* newCommandQueueInner() override final;
        virtual VkmPipelineStateBase* newPipelineStateInner() override final;
        virtual VkmRenderResourcePool* newRenderResourcePoolInner() override final;
        virtual VkmFormat selectSwapChainColorFormat(bool enableHdr) const override final;

    private:
        id<MTLDevice> _mtlDevice;
        std::vector<std::unique_ptr<VkmGpuHeapPoolMetal>> _heapPools;

        // Owned +1 under MRC, like _captureScope below.
        id<MTL4CounterHeap> _timestampCounterHeap {nullptr};
        double _timestampPeriodNs {1.0};

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