// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_command_buffer.h>

#include <Metal/MTLCommandBuffer.h>

namespace vkm
{
    void VkmCommandEncoderMetal::beginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc)
    {
        _mtlRenderPassDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        _mtlRenderPassDescriptor.colorAttachments[0].texture = (__bridge id<MTLTexture>)frameBufferDesc.colorAttachments[0].texture;
        _mtlRenderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
        _mtlRenderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
        _mtlRenderPassDescriptor.depthAttachment.texture = (__bridge id<MTLTexture>)frameBufferDesc.depthAttachment.texture;
        _mtlRenderPassDescriptor.depthAttachment.loadAction = MTLLoadActionClear;
        _mtlRenderPassDescriptor.depthAttachment.storeAction = MTLStoreActionStore;

        _mtlRenderCommandEncoder = [_mtlCommandBuffer renderCommandEncoderWithDescriptor:_mtlRenderPassDescriptor];

        _currentEncoderType = VkmCommandEncoderType::Graphics;
    }

    void VkmCommandEncoderMetal::beginComputePass()
    {
        _mtlComputeCommandEncoder = [_mtlCommandBuffer computeCommandEncoder];
        _currentEncoderType = VkmCommandEncoderType::Compute;
    }

    void VkmCommandEncoderMetal::beginBlitPass()
    {
        _mtlBlitCommandEncoder = [_mtlCommandBuffer blitCommandEncoder];
        _currentEncoderType = VkmCommandEncoderType::Blit;
    }

    void VkmCommandEncoderMetal::commit()
    {
        switch(_currentEncoderType)
        {
            case VkmCommandEncoderType::Render:
                [_mtlRenderCommandEncoder endEncoding];
                _mtlRenderCommandEncoder = nil;
                break;
            case VkmCommandEncoderType::Compute:
                [_mtlComputeCommandEncoder endEncoding];
                _mtlComputeCommandEncoder = nil;
                break;
            case VkmCommandEncoderType::Blit:
                [_mtlBlitCommandEncoder endEncoding];
                _mtlBlitCommandEncoder = nil;
                break;
            default:
                break;
        }
    }

    void VkmCommandEncoderMetal::reset()
    {
        _currentEncoderType = VkmCommandEncoderType::None;
    }

    VkmCommandBufferMetal::VkmCommandBufferMetal(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool)
        : VkmCommandBufferBase(driver, commandQueue, commandBufferPool)
    {
    }

    VkmCommandBufferMetal::~VkmCommandBufferMetal()
    {
    }

    void VkmCommandBufferMetal::setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle)
    {
        _mtlCommandBuffer = (__bridge id<MTLCommandBuffer>)handle;
        _commandEncoder.setMTLCommandBuffer(_mtlCommandBuffer);
    }

    void VkmCommandBufferMetal::onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc)
    {
        _commandEncoder.beginRenderPass(frameBufferDesc);
    }

    void VkmCommandBufferMetal::onEndRenderPass()
    {
        _commandEncoder.commit();
    }
}