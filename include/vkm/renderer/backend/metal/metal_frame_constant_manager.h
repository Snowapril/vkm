// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/frame_constants.h>

#include <cstdint>

@protocol MTLBuffer;

namespace vkm
{
    class VkmDriverMetal;

    // Metal implementation of the engine-global "set 1" per-frame constant convention (see
    // common/frame_constants.h). Metal has no descriptor sets, and vkm-compiler declares set 1
    // discrete so the generated MSL takes it as a plain `constant VkmFrameConstants&` at
    // [[buffer(kVkmMetalFrameConstantBufferIndex)]] -- exactly how push constants already
    // reach [[buffer(3)]]. So all this owns is one Shared-storage buffer holding FRAME_COUNT
    // regions, and binding is a single setAddress: of the active region.
    class VkmFrameConstantManagerMetal : public VkmFrameConstantManagerBase
    {
    public:
        explicit VkmFrameConstantManagerMetal(VkmDriverMetal* driver);
        ~VkmFrameConstantManagerMetal();

        bool initialize();
        void destroy() override final;

        void update(uint32_t frameIndex, const VkmFrameConstants& constants) override final;

        // GPU address of the region the most recent update() wrote, to be bound at
        // kVkmMetalFrameConstantBufferIndex.
        uint64_t getActiveGpuAddress() const;

    private:
        VkmDriverMetal* _driver;
        id<MTLBuffer> _constantBuffer = nullptr; // FRAME_COUNT x kVkmFrameConstantStride, shared storage
    };
} // namespace vkm
