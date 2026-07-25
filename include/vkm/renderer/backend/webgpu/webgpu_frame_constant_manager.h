// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/frame_constants.h>

#include <webgpu/webgpu.h>

#include <array>
#include <cstdint>

namespace vkm
{
    class VkmDriverWebGPU;

    // Owns the engine-wide "bind group 1" per-frame constant set (see common/frame_constants.h):
    // one uniform buffer carved into FRAME_COUNT regions plus one bind group per region. Every
    // WebGPU pipeline declares this layout as group 1 alongside the bindless group 0 (see
    // VkmPipelineStateWebGPU::createInner), and every graphics pipeline bind sets it -- WebGPU
    // requires every group a pipeline layout declares to be set before a draw, whether or not
    // the shader references it.
    //
    // Unlike group 0's push-constant emulation this uses a static offset per bind group rather
    // than one dynamic-offset group: the contents change once per frame, not per draw, so there
    // is no offset for a call site to keep in sync.
    class VkmFrameConstantManagerWebGPU : public VkmFrameConstantManagerBase
    {
    public:
        explicit VkmFrameConstantManagerWebGPU(VkmDriverWebGPU* driver);
        ~VkmFrameConstantManagerWebGPU();

        bool initialize();
        void destroy() override final;

        void update(uint32_t frameIndex, const VkmFrameConstants& constants) override final;

        inline WGPUBindGroupLayout getBindGroupLayout() const { return _bindGroupLayout; }

        // Bind group covering the region the most recent update() wrote.
        inline WGPUBindGroup getActiveBindGroup() const { return _bindGroups[_activeFrameIndex]; }

    private:
        VkmDriverWebGPU* _driver;

        WGPUBuffer _buffer = nullptr; // FRAME_COUNT x kVkmFrameConstantStride, Uniform | CopyDst
        WGPUBindGroupLayout _bindGroupLayout = nullptr;
        std::array<WGPUBindGroup, FRAME_COUNT> _bindGroups{};
    };
} // namespace vkm
