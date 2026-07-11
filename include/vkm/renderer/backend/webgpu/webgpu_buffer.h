// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/buffer.h>
#include <webgpu/webgpu.h>

namespace vkm
{
    class VkmBufferWebGPU final : public VkmBuffer
    {
    public:
        VkmBufferWebGPU(VkmDriverBase* driver);
        ~VkmBufferWebGPU();

        virtual bool initialize(VkmResourceHandle handle, const VkmBufferInfo& info) override final;
        virtual bool overrideExternalHandle(void* externalHandle) override final;
        virtual void setDebugName(const char* name) override final;

        inline WGPUBuffer getBuffer() const { return _wgpuBuffer; }

    private:
        WGPUBuffer _wgpuBuffer{nullptr};
        bool _externallyOwned{false};
    };
} // namespace vkm
