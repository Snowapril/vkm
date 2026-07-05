// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>
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

    protected:
        virtual VkmInitResult initializeInner(const VkmEngineLaunchOptions* options) override final;
        virtual void destroyInner() override final;
        virtual VkmTexture* newTextureInner() override final;
        virtual VkmCommandQueueBase* newCommandQueueInner() override final;

    private:
        WGPUInstance _instance{nullptr};
        WGPUAdapter  _adapter{nullptr};
        WGPUDevice   _device{nullptr};
        WGPUQueue    _queue{nullptr};
    };
} // namespace vkm
