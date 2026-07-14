// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/metal/metal_command_buffer.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_pipeline_state.h>
#include <vkm/renderer/backend/metal/metal_staging_buffer.h>

#import <Metal/MTL4CommandBuffer.h>
#import <Metal/MTL4RenderCommandEncoder.h>
#import <Metal/MTL4ComputeCommandEncoder.h>
#import <Metal/MTL4RenderPass.h>

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
        MTL4RenderPassDescriptor* mtlRenderPassDescriptor = [[MTL4RenderPassDescriptor alloc] init];
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
        _mtlCommandBuffer = (__bridge id<MTL4CommandBuffer>)handle;
        _commandEncoder.setMTLCommandBuffer(_mtlCommandBuffer);
#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        // Command buffers are pooled/reused -- discard any writes queued during a previous use.
        _pendingMarkerWrites.clear();
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS
    }

    void VkmCommandBufferMetal::onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc)
    {
        _commandEncoder.beginRenderPass(_driver->getRenderResourcePool(), frameBufferDesc);
    }

    void VkmCommandBufferMetal::onEndRenderPass()
    {
        _commandEncoder.commit();
    }

    void VkmCommandBufferMetal::onBindPipeline(VkmPipelineStateBase* pipelineState)
    {
        VkmPipelineStateMetal* pipelineStateMetal = static_cast<VkmPipelineStateMetal*>(pipelineState);
        if (pipelineStateMetal->isCompute())
        {
            [_commandEncoder.getActiveComputeCommandEncoder() setComputePipelineState:pipelineStateMetal->getComputePipelineState()];
        }
        else
        {
            id<MTL4RenderCommandEncoder> renderCommandEncoder = _commandEncoder.getActiveRenderCommandEncoder();
            [renderCommandEncoder setRenderPipelineState:pipelineStateMetal->getRenderPipelineState()];
            [renderCommandEncoder setDepthStencilState:pipelineStateMetal->getDepthStencilState()];
        }
    }

    void VkmCommandBufferMetal::onUnbindPipeline()
    {
        // No explicit "unbind" concept in Metal -- the next bindPipeline() call (or the
        // end of the render/compute pass) supersedes whatever pipeline state is set.
    }

    void VkmCommandBufferMetal::onCopyBuffer(VkmResourceHandle, VkmResourceHandle, uint64_t, uint64_t, uint64_t)
    {
        // Not implemented yet on Metal -- bindless resource upload is Vulkan-only for now.
        VKM_DEBUG_ERROR("VkmCommandBufferMetal::onCopyBuffer is not implemented");
    }

    void VkmCommandBufferMetal::onDraw(uint32_t, uint32_t, uint32_t, uint32_t)
    {
        // Not implemented yet on Metal -- bindless draw-call recording is Vulkan-only for now.
        VKM_DEBUG_ERROR("VkmCommandBufferMetal::onDraw is not implemented");
    }

    void VkmCommandBufferMetal::onSetPushConstants(const void*, uint32_t, uint32_t)
    {
        // Not implemented yet on Metal -- bindless push constants are Vulkan-only for now.
        VKM_DEBUG_ERROR("VkmCommandBufferMetal::onSetPushConstants is not implemented");
    }

    void VkmCommandBufferMetal::onSetDebugName(const char* name)
    {
        _mtlCommandBuffer.label = [NSString stringWithUTF8String:name];
    }

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
    void VkmCommandBufferMetal::onWriteCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t offset)
    {
        // Queued, not recorded immediately -- see onEndCommandBuffer() and its doc comment in
        // command_buffer.h for why opening/closing a compute encoder per call is avoided here.
        _pendingMarkerWrites.push_back(PendingMarkerWrite{markerBuffer, oneBuffer, offset});
    }

    void VkmCommandBufferMetal::onEndCommandBuffer()
    {
        if (_pendingMarkerWrites.empty())
        {
            return;
        }

        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();

        // Metal4 has no blit encoder at all -- buffer copies live on the compute encoder.
        // Batches every writeCompletionMarker() call from this command buffer's recording into
        // a single compute pass, opened/closed once here rather than once per call.
        _commandEncoder.beginComputePass();
        id<MTL4ComputeCommandEncoder> computeEncoder = _commandEncoder.getActiveComputeCommandEncoder();
        for (const PendingMarkerWrite& write : _pendingMarkerWrites)
        {
            id<MTLBuffer> mtlMarkerBuffer = static_cast<VkmStagingBufferMetal*>(renderResourcePool->getResource<VkmStagingBuffer>(write.markerBuffer))->getBuffer();
            id<MTLBuffer> mtlOneBuffer = static_cast<VkmStagingBufferMetal*>(renderResourcePool->getResource<VkmStagingBuffer>(write.oneBuffer))->getBuffer();
            [computeEncoder copyFromBuffer:mtlOneBuffer
                               sourceOffset:0
                                   toBuffer:mtlMarkerBuffer
                          destinationOffset:write.offset
                                       size:sizeof(uint32_t)];
        }
        _commandEncoder.commit();

        _pendingMarkerWrites.clear();
    }
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS
}
