// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/upscaler.h>

@protocol MTL4FXTemporalScaler;

namespace vkm
{
    /*
    * @brief MetalFX temporal upscaler: wraps an MTL4FXTemporalScaler behind VkmUpscalerBase.
    * @details Encodes at command-buffer level through the scaler's own internal encoders. The
    * scaler's fence property carries the render graph's ordering: the encode bridges the
    * subgraph's producer waits and fence publish through
    * VkmCommandBufferMetal::bridgeExternalEncode().
    */
    class VkmUpscalerMetal final : public VkmUpscalerBase
    {
    public:
        VkmUpscalerMetal() = default;
        ~VkmUpscalerMetal() override = default;

    protected:
        virtual bool initializeInner() override final;
        virtual void destroyInner() override final;
        virtual void encodeInner(VkmCommandBufferBase* commandBuffer,
                                 const VkmUpscalerDispatchDesc& dispatchDesc) override final;

    private:
        id<MTL4FXTemporalScaler> _scaler = nullptr;
    };
} // namespace vkm
