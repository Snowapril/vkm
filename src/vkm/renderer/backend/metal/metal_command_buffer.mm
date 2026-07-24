// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/metal/metal_command_buffer.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_pipeline_state.h>
#include <vkm/renderer/backend/metal/metal_buffer.h>
#include <vkm/renderer/backend/metal/metal_staging_buffer.h>

#import <Metal/MTL4CommandBuffer.h>
#import <Metal/MTL4RenderCommandEncoder.h>
#import <Metal/MTL4ComputeCommandEncoder.h>
#import <Metal/MTL4RenderPass.h>
#import <Metal/MTL4ArgumentTable.h>
#import <Metal/MTLBuffer.h>

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

    static MTLPrimitiveType getPrimitiveType(VkmPrimitiveTopology topology)
    {
        switch (topology)
        {
            case VkmPrimitiveTopology::PointList:     return MTLPrimitiveTypePoint;
            case VkmPrimitiveTopology::LineList:      return MTLPrimitiveTypeLine;
            case VkmPrimitiveTopology::LineStrip:     return MTLPrimitiveTypeLineStrip;
            case VkmPrimitiveTopology::TriangleList:  return MTLPrimitiveTypeTriangle;
            case VkmPrimitiveTopology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
        }
        return MTLPrimitiveTypeTriangle;
    }

    // copyBuffer() accepts either a Buffer or a StagingBuffer resource on either side (see
    // the same-named helper in vulkan_command_buffer.cpp) -- resolve via the handle's own
    // recorded type. Metal buffers are never sub-allocated within a shared MTLBuffer, so
    // no extra base offset applies on either kind.
    static id<MTLBuffer> resolveMTLBuffer(VkmRenderResourcePool* renderResourcePool, VkmResourceHandle handle)
    {
        if (handle.type == VkmResourceType::StagingBuffer)
        {
            return static_cast<VkmStagingBufferMetal*>(renderResourcePool->getResource<VkmStagingBuffer>(handle))->getBuffer();
        }
        return static_cast<VkmBufferMetal*>(renderResourcePool->getResource<VkmBuffer>(handle))->getBuffer();
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
            _boundPrimitiveType = static_cast<uint32_t>(
                getPrimitiveType(pipelineStateMetal->getDescriptor().primitiveTopology));

            // Every graphics pipeline shares the engine-global bindless argument table:
            // the set-0 argument buffer at [[buffer(2)]] and a safe default for the
            // push-constant slot at [[buffer(3)]] (overwritten per setPushConstants call).
            // Mirrors VkmCommandBufferVulkan::onBindPipeline's implicit set-0 bind.
            VkmBindlessResourceManagerMetal* bindlessManager =
                static_cast<VkmDriverMetal*>(_driver)->getBindlessResourceManager();
            id<MTL4ArgumentTable> argumentTable = bindlessManager->getArgumentTable();
            [renderCommandEncoder setArgumentTable:argumentTable
                                          atStages:MTLRenderStageVertex | MTLRenderStageFragment];
            [argumentTable setAddress:bindlessManager->getArgumentBuffer().gpuAddress
                              atIndex:kVkmMetalBindlessArgumentBufferIndex];
        }
    }

    void VkmCommandBufferMetal::onUnbindPipeline()
    {
        // No explicit "unbind" concept in Metal -- the next bindPipeline() call (or the
        // end of the render/compute pass) supersedes whatever pipeline state is set.
    }

    void VkmCommandBufferMetal::onCopyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        id<MTLBuffer> mtlSrcBuffer = resolveMTLBuffer(renderResourcePool, srcBuffer);
        id<MTLBuffer> mtlDstBuffer = resolveMTLBuffer(renderResourcePool, dstBuffer);

        // Metal4 has no blit encoder -- buffer copies live on the compute encoder. The
        // base class guarantees copyBuffer() is only called outside a render pass, and it
        // is a setup-time upload path, so a per-call encoder is acceptable here; do NOT
        // leave a compute encoder open across recording (see onEndCommandBuffer's batched
        // marker writes and the queue-timeout lesson behind them).
        _commandEncoder.beginComputePass();
        id<MTL4ComputeCommandEncoder> computeEncoder = _commandEncoder.getActiveComputeCommandEncoder();
        // Metal4 does no automatic hazard tracking: wait for prior encoders that may have
        // written the source (e.g. render graph capture copying a buffer a pass just used),
        // and make the copy visible to later encoders.
        [computeEncoder barrierAfterQueueStages:MTLStageAll
                                   beforeStages:MTLStageBlit
                              visibilityOptions:MTL4VisibilityOptionDevice];
        [computeEncoder copyFromBuffer:mtlSrcBuffer
                          sourceOffset:srcOffset
                              toBuffer:mtlDstBuffer
                     destinationOffset:dstOffset
                                  size:size];
        [computeEncoder barrierAfterStages:MTLStageBlit
                         beforeQueueStages:MTLStageAll
                         visibilityOptions:MTL4VisibilityOptionDevice];
        _commandEncoder.commit();
    }

    void VkmCommandBufferMetal::onCopyTexture(VkmResourceHandle srcTexture, VkmResourceHandle dstTexture)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        VkmTextureMetal* srcTextureMetal = static_cast<VkmTextureMetal*>(renderResourcePool->getResource<VkmTexture>(srcTexture));
        VkmTextureMetal* dstTextureMetal = static_cast<VkmTextureMetal*>(renderResourcePool->getResource<VkmTexture>(dstTexture));
        const VkmTextureInfo& srcInfo = srcTextureMetal->getTextureInfo();

        // Metal4 has no blit encoder -- texture copies live on the compute encoder, same
        // per-call encoder tradeoff as onCopyBuffer above (debug-capture path, not per-frame).
        _commandEncoder.beginComputePass();
        id<MTL4ComputeCommandEncoder> computeEncoder = _commandEncoder.getActiveComputeCommandEncoder();
        // Metal4 does no automatic hazard tracking: wait for prior encoders (e.g. the render
        // pass that wrote the source) before copying, and make the copy visible to later
        // encoders (e.g. an ImGui pass sampling the destination).
        [computeEncoder barrierAfterQueueStages:MTLStageAll
                                   beforeStages:MTLStageBlit
                              visibilityOptions:MTL4VisibilityOptionDevice];
        [computeEncoder copyFromTexture:srcTextureMetal->getInternalHandle()
                            sourceSlice:0
                            sourceLevel:0
                           sourceOrigin:MTLOriginMake(0, 0, 0)
                             sourceSize:MTLSizeMake(srcInfo._extent.x, srcInfo._extent.y, srcInfo._extent.z)
                              toTexture:dstTextureMetal->getInternalHandle()
                       destinationSlice:0
                       destinationLevel:0
                      destinationOrigin:MTLOriginMake(0, 0, 0)];
        [computeEncoder barrierAfterStages:MTLStageBlit
                         beforeQueueStages:MTLStageAll
                         visibilityOptions:MTL4VisibilityOptionDevice];
        _commandEncoder.commit();
    }

    void VkmCommandBufferMetal::onCopyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        VkmTextureMetal* textureMetal = static_cast<VkmTextureMetal*>(renderResourcePool->getResource<VkmTexture>(srcTexture));
        id<MTLBuffer> mtlDstBuffer = resolveMTLBuffer(renderResourcePool, dstBuffer);

        const VkmTextureInfo& textureInfo = textureMetal->getTextureInfo();
        const uint32_t bytesPerRow = textureInfo._extent.x * vkmBytesPerTexel(textureInfo._format);

        // Metal4 has no blit encoder -- texture copies live on the compute encoder. Same
        // per-call encoder rationale as onCopyBuffer above; barriers because Metal4 does no
        // automatic hazard tracking (a same-command-buffer copy right after the render pass
        // that wrote the source, e.g. render graph capture, reads garbage without them).
        _commandEncoder.beginComputePass();
        id<MTL4ComputeCommandEncoder> computeEncoder = _commandEncoder.getActiveComputeCommandEncoder();
        [computeEncoder barrierAfterQueueStages:MTLStageAll
                                   beforeStages:MTLStageBlit
                              visibilityOptions:MTL4VisibilityOptionDevice];
        [computeEncoder copyFromTexture:textureMetal->getInternalHandle()
                            sourceSlice:0
                            sourceLevel:0
                           sourceOrigin:MTLOriginMake(0, 0, 0)
                             sourceSize:MTLSizeMake(textureInfo._extent.x, textureInfo._extent.y, 1)
                               toBuffer:mtlDstBuffer
                      destinationOffset:dstOffset
                 destinationBytesPerRow:bytesPerRow
               destinationBytesPerImage:0];
        [computeEncoder barrierAfterStages:MTLStageBlit
                         beforeQueueStages:MTLStageAll
                         visibilityOptions:MTL4VisibilityOptionDevice];
        _commandEncoder.commit();
    }

    void VkmCommandBufferMetal::onDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        [_commandEncoder.getActiveRenderCommandEncoder() drawPrimitives:static_cast<MTLPrimitiveType>(_boundPrimitiveType)
                                                            vertexStart:firstVertex
                                                            vertexCount:vertexCount
                                                          instanceCount:instanceCount
                                                           baseInstance:firstInstance];
    }

    void VkmCommandBufferMetal::onSetPushConstants(const void* data, uint32_t size, uint32_t offset)
    {
        // Each call fills a fresh ring entry, so a partial update at a non-zero offset
        // cannot preserve previously-pushed bytes; no caller needs that today.
        VKM_ASSERT(offset == 0, "Metal push constants only support offset 0");

        VkmBindlessResourceManagerMetal* bindlessManager =
            static_cast<VkmDriverMetal*>(_driver)->getBindlessResourceManager();
        const uint64_t gpuAddress = bindlessManager->allocatePushConstantSlot(data, size);
        // Argument-table values are captured at each encoded draw, so mutating the shared
        // table between draws is safe (the ImGui Metal renderer relies on the same rule).
        [bindlessManager->getArgumentTable() setAddress:gpuAddress
                                                atIndex:kVkmMetalPushConstantBufferIndex];
    }

    void VkmCommandBufferMetal::onSetDebugName(const char* name)
    {
        _mtlCommandBuffer.label = [NSString stringWithUTF8String:name];
    }

    void VkmCommandBufferMetal::onPushDebugGroup(const char* name)
    {
        // Command-buffer scope (not encoder scope): the render/compute encoders are nil between
        // passes, and this is called from VkmRenderGraph::execute() outside any render pass.
        [_mtlCommandBuffer pushDebugGroup:[NSString stringWithUTF8String:name]];
    }

    void VkmCommandBufferMetal::onPopDebugGroup()
    {
        [_mtlCommandBuffer popDebugGroup];
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
