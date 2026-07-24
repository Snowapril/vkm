// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_bindless_resource_manager.h>
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
    };
} // namespace vkm
