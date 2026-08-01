// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_bindless_resource_manager.h>
#include <vkm/renderer/backend/webgpu/webgpu_frame_constant_manager.h>
#include <webgpu/webgpu.h>

namespace vkm
{
    /*
    * @brief renderer backend driver for WebGPU (Emscripten/WASM only)
    */
    class VkmDriverWebGPU final : public VkmDriverBase
    {
    public:
        VkmDriverWebGPU();
        ~VkmDriverWebGPU();

        virtual VkmSwapChainBase* newSwapChainInner() override final;
        virtual VkmPerPassResourceTableBase* newPerPassResourceTableInner() override final;

        inline WGPUInstance getInstance() const { return _instance; }
        inline WGPUAdapter getAdapter() const { return _adapter; }
        inline WGPUDevice getDevice() const { return _device; }
        inline WGPUQueue getQueue() const { return _queue; }

        // Shadows VkmDriverBase::getBindlessResourceManager() with the WebGPU-typed manager
        // (the base member always holds a VkmBindlessResourceManagerWebGPU for this driver).
        inline VkmBindlessResourceManagerWebGPU* getBindlessResourceManager() const
        {
            return static_cast<VkmBindlessResourceManagerWebGPU*>(_bindlessResourceManager.get());
        }

        // Shadows VkmDriverBase::getFrameConstantManager() with the WebGPU-typed manager, for
        // the same reason as above.
        inline VkmFrameConstantManagerWebGPU* getFrameConstantManager() const
        {
            return static_cast<VkmFrameConstantManagerWebGPU*>(_frameConstantManager.get());
        }

        /*
        * @brief GPU timestamp pool backing VkmGpuProfiler: one WGPUQuerySet of `slotCount`
        * timestamp queries, plus the two buffers a query set can only be read through (a
        * QueryResolve target the GPU writes and a MapRead copy the CPU reads). Only available
        * when the adapter offers the optional "timestamp-query" feature, which is requested at
        * device creation.
        *
        * WebGPU has no encoder-level timestamp write, so unlike Vulkan/Metal the writes
        * themselves are attached to render/compute pass descriptors -- see
        * VkmCommandBufferWebGPU::onBeginGpuZone.
        */
        virtual bool initializeGpuTimestampPool(uint32_t slotCount) override final;
        virtual void destroyGpuTimestampPool() override final;
        virtual bool resolveGpuTimestamps(uint32_t firstSlot, uint32_t count, uint64_t* outTicks) override final;

        inline WGPUQuerySet getGpuTimestampQuerySet() const { return _timestampQuerySet; }
        inline WGPUBuffer getGpuTimestampResolveBuffer() const { return _timestampResolveBuffer; }
        inline WGPUBuffer getGpuTimestampReadbackBuffer() const { return _timestampReadbackBuffer; }

    protected:
        virtual VkmInitResult initializeInner(const VkmEngineLaunchOptions* options) override final;
        virtual bool postInitializeInner() override final;
        virtual void destroyInner() override final;
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
        WGPUInstance _instance{nullptr};
        WGPUAdapter  _adapter{nullptr};
        WGPUDevice   _device{nullptr};
        WGPUQueue    _queue{nullptr};

        WGPUQuerySet _timestampQuerySet{nullptr};
        WGPUBuffer   _timestampResolveBuffer{nullptr};
        WGPUBuffer   _timestampReadbackBuffer{nullptr};
        // Requested at device creation; without it wgpuDeviceCreateQuerySet(Timestamp) is invalid.
        bool         _timestampQuerySupported{false};
    };
} // namespace vkm
