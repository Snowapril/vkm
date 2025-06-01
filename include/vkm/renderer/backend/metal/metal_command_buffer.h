// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_buffer.h>

@protocol MTLCommandBuffer;

namespace vkm
{
    enum class VkmCommandEncoderType : uint8_t
    {
        None = 0,
        Graphics,
        Compute,
        Blit
    };

    class VkmCommandEncoderMetal
    {
    public:
        VkmCommandEncoderMetal() = default;
        ~VkmCommandEncoderMetal() = default;
    
        void setMTLCommandBuffer(id<MTLCommandBuffer> mtlCommandBuffer)
        {
            _mtlCommandBuffer = mtlCommandBuffer;
        }

        void beginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc);
        void beginComputePass();
        void beginBlitPass();
        void commit();
        void reset();

    private:
        id<MTLCommandBuffer> _mtlCommandBuffer; // Metal command buffer

        MTLRenderCommandEncoder* _mtlRenderCommandEncoder = nullptr; // Metal render command encoder
        MTLComputeCommandEncoder* _mtlComputeCommandEncoder = nullptr; // Metal compute command encoder
        MTLBlitCommandEncoder* _mtlBlitCommandEncoder = nullptr; // Metal blit command encoder

        VkmCommandEncoderType _currentEncoderType = VkmCommandEncoderType::None; // Current encoder type
    }

    class VkmCommandBufferMetal : public VkmCommandBufferBase
    {
        friend class VkmCommandBufferPoolMetal;
    public:
        VkmCommandBufferMetal(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool);
        ~VkmCommandBufferMetal();

        virtual void setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle) override final;

        virtual void onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc) override final;
        virtual void onEndRenderPass() override final;

    private:
        id<MTLCommandBuffer> _mtlCommandBuffer;
        VkmCommandEncoderMetal _commandEncoder; // Command encoder for Metal
    };
}