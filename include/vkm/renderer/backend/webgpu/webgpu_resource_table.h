// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/resource_table.h>

#include <webgpu/webgpu.h>

namespace vkm
{
    /*
    * @brief WebGPU set 2: one WGPUBindGroup, created once from the pipeline's group-2 layout.
    *
    * @details The base class's immutability matches WebGPU's own model, where a bind group is an
    * immutable object you recreate rather than rewrite.
    * This matters beyond per-pass resources: WebGPU cannot sample any texture through the engine's
    * bindless path, which needs the runtime-sized arrays WGSL lacks, so group 2's fixed bindings
    * are the only way a shader samples a texture on this backend.
    */
    class VkmResourceTableWebGPU : public VkmResourceTableBase
    {
    public:
        explicit VkmResourceTableWebGPU(VkmDriverBase* driver);
        ~VkmResourceTableWebGPU() override;

        inline WGPUBindGroup getBindGroup() const { return _bindGroup; }

    protected:
        bool createInner(const std::vector<VkmTableResourceEntry>& entries, std::string* outError) override final;
        void destroyInner() override final;

    private:
        WGPUBindGroup _bindGroup{nullptr};
        // Texture views are created for the bind group and must outlive it.
        std::vector<WGPUTextureView> _textureViews;
    };
} // namespace vkm
