// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/texture_view.h>
#include <webgpu/webgpu.h>

namespace vkm
{
    class VkmTextureViewWebGPU final : public VkmTextureView
    {
    public:
        VkmTextureViewWebGPU(VkmDriverBase* driver);
        ~VkmTextureViewWebGPU();

        virtual bool initialize(VkmResourceHandle handle, const VkmTextureViewInfo& info) override final;
        virtual void setDebugName(const char* name) override final;

        inline WGPUTextureView getTextureView() const { return _wgpuTextureView; }

    private:
        WGPUTextureView _wgpuTextureView{nullptr};
    };
} // namespace vkm
