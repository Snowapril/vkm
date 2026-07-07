// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_buffer.h>

@protocol MTL4CommandBuffer;
@protocol MTL4RenderCommandEncoder;
@protocol MTL4ComputeCommandEncoder;

namespace vkm
{
    class VkmRenderResourcePool;
    enum class VkmCommandEncoderType : uint8_t
    {
        None = 0,
        Graphics,
        Compute,
    };

    class VkmCommandEncoderMetal
    {
    public:
        VkmCommandEncoderMetal() = default;
        ~VkmCommandEncoderMetal() = default;

        void setMTLCommandBuffer(id<MTL4CommandBuffer> mtlCommandBuffer)
        {
            _mtlCommandBuffer = mtlCommandBuffer;
        }

        void beginRenderPass(VkmRenderResourcePool* renderResourcePool, const VkmFrameBufferDescriptor& frameBufferDesc);
        void beginComputePass();
        void commit();
        void reset();

        inline id<MTL4RenderCommandEncoder> getActiveRenderCommandEncoder() const { return _mtlRenderCommandEncoder; }

    private:
        id<MTL4CommandBuffer> _mtlCommandBuffer;

        id<MTL4RenderCommandEncoder> _mtlRenderCommandEncoder = nullptr;
        id<MTL4ComputeCommandEncoder> _mtlComputeCommandEncoder = nullptr;

        VkmCommandEncoderType _currentEncoderType = VkmCommandEncoderType::None;
    };

    class VkmCommandBufferMetal : public VkmCommandBufferBase
    {
        friend class VkmCommandBufferPoolMetal;
    public:
        VkmCommandBufferMetal(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool);
        ~VkmCommandBufferMetal();

        virtual void setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle) override final;

        virtual void onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc) override final;
        virtual void onEndRenderPass() override final;

        inline id<MTL4CommandBuffer> getMTLCommandBuffer() const { return _mtlCommandBuffer; }
        inline id<MTL4RenderCommandEncoder> getActiveRenderCommandEncoder() const { return _commandEncoder.getActiveRenderCommandEncoder(); }

    private:
        id<MTL4CommandBuffer> _mtlCommandBuffer;
        VkmCommandEncoderMetal _commandEncoder;
    };
}
