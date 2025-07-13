// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/metal/metal_command_buffer.h>
#include <vkm/renderer/backend/metal/metal_texture.h>

#include <Metal/MTLCommandBuffer.h>
#include <Metal/MTLRenderCommandEncoder.h>
#include <Metal/MTLComputeCommandEncoder.h>
#include <Metal/MTLBlitCommandEncoder.h>

namespace vkm
{
    static MTLLoadAction getLoadAction(VkmLoadAction loadAction)
    {
        switch(loadAction)
        {
            case VkmLoadAction::Load:
                return MTLLoadActionLoad;
            case VkmLoadAction::Clear:
                return MTLLoadActionClear;
            case VkmLoadAction::DontCare:
                return MTLLoadActionDontCare;
        }
    }

    static MTLStoreAction getStoreAction(VkmStoreAction storeAction)
    {
        switch(storeAction)
        {
            case VkmStoreAction::Store:
                return MTLStoreActionStore;
            case VkmStoreAction::DontCare:
                return MTLStoreActionDontCare;
        }
    }

    void VkmCommandEncoderMetal::beginRenderPass(VkmRenderResourcePool* renderResourcePool, const VkmFrameBufferDescriptor& frameBufferDesc)
    {
        MTLRenderPassDescriptor* mtlRenderPassDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        const VkmRenderPassDescriptor& renderPassDesc = frameBufferDesc._renderPass;
        for (uint32_t i = 0; i < renderPassDesc._colorAttachmentCount; ++i)
        {
            auto colorAttachmentHandle = frameBufferDesc._colorAttachments[i];
            VkmTextureMetal* colorTextureMetal = static_cast<VkmTextureMetal*>(renderResourcePool->getResource<VkmTexture>(colorAttachmentHandle));
            
            const VkmColorAttachmentDescriptor& colorAttachmentDesc = renderPassDesc._colorAttachments[i];
            mtlRenderPassDescriptor.colorAttachments[i].texture = colorTextureMetal->getInternalHandle();
            mtlRenderPassDescriptor.colorAttachments[i].loadAction = getLoadAction(colorAttachmentDesc._loadAction);
            mtlRenderPassDescriptor.colorAttachments[i].storeAction = getStoreAction(colorAttachmentDesc._storeAction);
            mtlRenderPassDescriptor.colorAttachments[i].clearColor = MTLClearColorMake( colorAttachmentDesc._clearColors[0],
                                                                                       colorAttachmentDesc._clearColors[1],
                                                                                       colorAttachmentDesc._clearColors[2],
                                                                                       colorAttachmentDesc._clearColors[3]);
        }

        if (frameBufferDesc._depthStencilAttachment.has_value())
        {
            VkmTextureMetal* depthStencilTextureMetal = static_cast<VkmTextureMetal*>(renderResourcePool->getResource<VkmTexture>(frameBufferDesc._depthStencilAttachment.value()));;
            const VkmTextureInfo& textureInfo = depthStencilTextureMetal->getTextureInfo();

            if (hasDepth(textureInfo._format))
            {
                mtlRenderPassDescriptor.depthAttachment.texture = depthStencilTextureMetal->getInternalHandle();
                mtlRenderPassDescriptor.depthAttachment.loadAction = MTLLoadActionClear;
                mtlRenderPassDescriptor.depthAttachment.storeAction = MTLStoreActionStore;
            }
            
            if (hasStencil(textureInfo._format))
            {
                mtlRenderPassDescriptor.stencilAttachment.texture = depthStencilTextureMetal->getInternalHandle();
                mtlRenderPassDescriptor.stencilAttachment.loadAction = MTLLoadActionClear;
                mtlRenderPassDescriptor.stencilAttachment.storeAction = MTLStoreActionStore;
            }
        }

        _mtlRenderCommandEncoder = [_mtlCommandBuffer renderCommandEncoderWithDescriptor:mtlRenderPassDescriptor];
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
            case VkmCommandEncoderType::Graphics:
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
        _commandEncoder.beginRenderPass(_driver->getRenderResourcePool(), frameBufferDesc);
    }

    void VkmCommandBufferMetal::onEndRenderPass()
    {
        _commandEncoder.commit();
    }
}
