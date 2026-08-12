// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/metal/metal_command_buffer.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_pipeline_state.h>
#include <vkm/renderer/backend/metal/metal_acceleration_structure.h>
#include <vkm/renderer/backend/metal/metal_resource_table.h>
#include <vkm/renderer/backend/metal/metal_buffer.h>
#include <vkm/renderer/backend/metal/metal_staging_buffer.h>

#include <algorithm>

#import <Metal/MTL4CommandBuffer.h>
#import <Metal/MTL4Counters.h>
#import <Metal/MTL4RenderCommandEncoder.h>
#import <Metal/MTL4ComputeCommandEncoder.h>
#import <Metal/MTL4RenderPass.h>
#import <Metal/MTL4ArgumentTable.h>
#import <Metal/MTLBuffer.h>

namespace vkm
{
    /*
    * @brief The Metal stages an access runs in.
    * @details The scope decides the shader stages, because the access cannot: a sampled read is a
    * fragment read in a graphics subgraph and a dispatch read in a compute one. Accesses carrying
    * their own stage -- attachments, transfers, acceleration structure builds -- ignore it.
    * @param access How the work touches the resource.
    * @param scope Which pipeline the work runs on.
    * @return A stage mask, MTLStageAll where the stage is unknowable.
    */
    static MTLStages vkmToMTLStages(VkmResourceAccess access, VkmPipelineScope scope)
    {
        const MTLStages scopeStages = (scope == VkmPipelineScope::Graphics)
                                          ? (MTLStageVertex | MTLStageFragment)
                                          : (scope == VkmPipelineScope::Compute) ? MTLStageDispatch
                                          : (scope == VkmPipelineScope::Transfer) ? MTLStageBlit
                                                                                  : MTLStageAll;
        switch (access)
        {
            // Produced outside this render graph, so nothing here can narrow it.
            case VkmResourceAccess::None:
            case VkmResourceAccess::Present:
                return MTLStageAll;

            case VkmResourceAccess::IndirectArgument:
                return MTLStageVertex | MTLStageDispatch;

            case VkmResourceAccess::ConstantBufferRead:
            case VkmResourceAccess::ShaderSampledRead:
            case VkmResourceAccess::ShaderStorageRead:
            case VkmResourceAccess::ShaderStorageWrite:
            case VkmResourceAccess::ShaderStorageReadWrite:
            case VkmResourceAccess::AccelerationStructureShaderRead:
                return scopeStages;

            case VkmResourceAccess::ColorAttachmentWrite:
            case VkmResourceAccess::DepthStencilAttachmentWrite:
            case VkmResourceAccess::DepthStencilAttachmentRead:
                return MTLStageFragment;

            // Metal 4 has no blit encoder; copies run on a compute encoder at the blit stage.
            case VkmResourceAccess::TransferRead:
            case VkmResourceAccess::TransferWrite:
                return MTLStageBlit;

            case VkmResourceAccess::AccelerationStructureBuildRead:
            case VkmResourceAccess::AccelerationStructureBuildWrite:
                return MTLStageAccelerationStructure;

            default:
                return MTLStageAll;
        }
    }

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

    // VkmFillMode::Point and VkmCullMode::FrontAndBack are rejected at PSO creation
    // (see VkmPipelineStateMetal's raster-state validation), so they cannot reach here.
    static MTLTriangleFillMode getTriangleFillMode(VkmFillMode fillMode)
    {
        switch (fillMode)
        {
            case VkmFillMode::Solid:     return MTLTriangleFillModeFill;
            case VkmFillMode::Wireframe: return MTLTriangleFillModeLines;
            default:                     return MTLTriangleFillModeFill;
        }
    }

    static MTLCullMode getCullMode(VkmCullMode cullMode)
    {
        switch (cullMode)
        {
            case VkmCullMode::None:  return MTLCullModeNone;
            case VkmCullMode::Front: return MTLCullModeFront;
            case VkmCullMode::Back:  return MTLCullModeBack;
            default:                 return MTLCullModeBack;
        }
    }

    // Mapped 1:1, unlike vulkan_pipeline_state.cpp's toVkFrontFace, which is deliberately
    // inverted. The engine's clip space is +Y-up, which is already Metal's convention; Vulkan
    // is the one backend that compensates, because vkm-compiler builds its SPIR-V with
    // -fvk-invert-y and that mirrors triangle winding in screen space. WebGPU maps 1:1 for the
    // same reason Metal does.
    static MTLWinding getFrontFacingWinding(VkmFrontFace frontFace)
    {
        switch (frontFace)
        {
            case VkmFrontFace::CounterClockwise: return MTLWindingCounterClockwise;
            case VkmFrontFace::Clockwise:        return MTLWindingClockwise;
            default:                             return MTLWindingCounterClockwise;
        }
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

    // Every encoder-scoped stage mask -- both halves of a fence and the intra-encoder barrier --
    // may only name stages the encoder itself can encode; Metal rejects the rest outright. The two
    // sets are disjoint, so which one applies identifies the encoder type.
    static constexpr MTLStages kRenderEncodableStages =
        MTLStageVertex | MTLStageFragment | MTLStageTile | MTLStageObject | MTLStageMesh;
    static constexpr MTLStages kComputeEncodableStages =
        MTLStageDispatch | MTLStageBlit | MTLStageAccelerationStructure;

    // Whichever encoder is open, together with the stages it can name. Nil with an empty mask when
    // none is.
    static id<MTL4CommandEncoder> resolveEncoder(id<MTL4RenderCommandEncoder> renderEncoder,
                                                 id<MTL4ComputeCommandEncoder> computeEncoder,
                                                 MTLStages* outEncodableStages)
    {
        if (renderEncoder != nil)
        {
            *outEncodableStages = kRenderEncodableStages;
            return static_cast<id<MTL4CommandEncoder>>(renderEncoder);
        }
        if (computeEncoder != nil)
        {
            *outEncodableStages = kComputeEncodableStages;
            return static_cast<id<MTL4CommandEncoder>>(computeEncoder);
        }
        *outEncodableStages = 0;
        return nil;
    }

    void VkmCommandEncoderMetal::waitForProducers(const std::vector<id<MTLFence>>& fences,
                                                  uint64_t beforeEncoderStages)
    {
        MTLStages encodable = 0;
        id<MTL4CommandEncoder> encoder =
            resolveEncoder(_mtlRenderCommandEncoder, _mtlComputeCommandEncoder, &encodable);
        if (encoder == nil || fences.empty())
        {
            return;
        }
        // A dependency names its destination stages in graph terms, so it can name stages this
        // encoder has none of. Waiting before every stage the encoder does have orders more work
        // than the dependency asked for, never less.
        const MTLStages stages = (static_cast<MTLStages>(beforeEncoderStages) & encodable) ?: encodable;
        for (id<MTLFence> fence : fences)
        {
            [encoder waitForFence:fence beforeEncoderStages:stages];
        }
    }

    void VkmCommandEncoderMetal::flushAliasAcquireIfPending()
    {
        if (!_pendingAliasAcquire)
        {
            return;
        }
        _pendingAliasAcquire = false;

        MTLStages encodable = 0;
        id<MTL4CommandEncoder> encoder =
            resolveEncoder(_mtlRenderCommandEncoder, _mtlComputeCommandEncoder, &encodable);
        if (encoder == nil)
        {
            return;
        }
        /*
        * The queue form, not the encoder one: what has to be ordered against is every read of
        * the previous alias, which was encoded in earlier encoders and earlier frames -- nothing
        * in this encoder, which has encoded nothing yet. barrierAfterEncoderStages: would also
        * reject the mask outright, its afterStages accepting only the stages that can *produce*
        * within this encoder.
        *
        * MTL4VisibilityOptionResourceAlias is the point: it flushes the caches that make two
        * virtual addresses over the same bytes coherent. The ordinary barrier path never asks
        * for it, because nothing but aliasing needs it.
        */
        [encoder barrierAfterQueueStages:MTLStageAll
                            beforeStages:encodable
                       visibilityOptions:(MTL4VisibilityOptionDevice | MTL4VisibilityOptionResourceAlias)];
    }

    bool VkmCommandEncoderMetal::publishThroughFence(id<MTLFence> fence, uint64_t afterEncoderStages)
    {
        MTLStages encodable = 0;
        id<MTL4CommandEncoder> encoder =
            resolveEncoder(_mtlRenderCommandEncoder, _mtlComputeCommandEncoder, &encodable);
        if (encoder == nil || fence == nil)
        {
            return false;
        }
        // Same clamp as the wait half. Publishing after every stage the encoder has covers more
        // of this encoder's work than the release asked for, which is the safe direction.
        const MTLStages stages = (static_cast<MTLStages>(afterEncoderStages) & encodable) ?: encodable;
        [encoder updateFence:fence afterEncoderStages:stages];
        return true;
    }

    void VkmCommandEncoderMetal::beginRenderPass(VkmRenderResourcePool* renderResourcePool,
                                                 const VkmFrameBufferDescriptor& frameBufferDesc)
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

        // Both optionals are required: the handle lives on the framebuffer, the load/store
        // actions on the render pass, and they are set independently. Vulkan and WebGPU
        // already check both before dereferencing either.
        if (frameBufferDesc._depthStencilAttachment.has_value() && renderPassDesc._depthStencilAttachment.has_value())
        {
            VkmTextureMetal* depthStencilTextureMetal = static_cast<VkmTextureMetal*>(renderResourcePool->getResource<VkmTexture>(frameBufferDesc._depthStencilAttachment.value()));;
            const VkmTextureInfo& textureInfo = depthStencilTextureMetal->getTextureInfo();
            // One shared load/store pair covers both aspects, matching the descriptor (and
            // Vulkan, which builds a single attachment info and assigns it to both).
            const VkmDepthStencilAttachmentDescriptor& depthStencilDesc = renderPassDesc._depthStencilAttachment.value();

            if (hasDepth(textureInfo._format))
            {
                mtlRenderPassDescriptor.depthAttachment.texture = depthStencilTextureMetal->getInternalHandle();
                mtlRenderPassDescriptor.depthAttachment.loadAction = getLoadAction(depthStencilDesc._loadAction);
                mtlRenderPassDescriptor.depthAttachment.storeAction = getStoreAction(depthStencilDesc._storeAction);
                mtlRenderPassDescriptor.depthAttachment.clearDepth = depthStencilDesc._clearDepth;
            }

            if (hasStencil(textureInfo._format))
            {
                mtlRenderPassDescriptor.stencilAttachment.texture = depthStencilTextureMetal->getInternalHandle();
                mtlRenderPassDescriptor.stencilAttachment.loadAction = getLoadAction(depthStencilDesc._loadAction);
                mtlRenderPassDescriptor.stencilAttachment.storeAction = getStoreAction(depthStencilDesc._storeAction);
                mtlRenderPassDescriptor.stencilAttachment.clearStencil = depthStencilDesc._clearStencil;
            }
        }

        _mtlRenderCommandEncoder = [_mtlCommandBuffer renderCommandEncoderWithDescriptor:mtlRenderPassDescriptor];
        _currentEncoderType = VkmCommandEncoderType::Graphics;

        // These sources are compiled without ARC (see the [release] pairs elsewhere in the
        // Metal backend), and the encoder does not keep the descriptor -- without this the
        // descriptor and its attachment sub-objects leak once per render pass, i.e. every frame.
        [mtlRenderPassDescriptor release];
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
        releaseFences();
        [_mtlCommandBuffer release];
        _mtlCommandBuffer = nil;
    }

    void VkmCommandBufferMetal::setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle)
    {
        // getOrCreateRHICommandBuffer() hands over a +1 reference that the __bridge cast to
        // VKM_COMMAND_BUFFER_HANDLE cannot carry, so this slot owns it. Release the one from
        // the previous use rather than right after commit: by the time the pool hands this
        // slot out again its submission is a frame older, while the number of live command
        // buffers stays bounded by the pool size instead of growing every frame.
        id<MTL4CommandBuffer> previousCommandBuffer = _mtlCommandBuffer;

        _mtlCommandBuffer = (__bridge id<MTL4CommandBuffer>)handle;
        _commandEncoder.setMTLCommandBuffer(_mtlCommandBuffer);

        [previousCommandBuffer release];
#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        // Command buffers are pooled/reused -- discard any writes queued during a previous use.
        _pendingMarkerWrites.clear();
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS
    }

    void VkmCommandBufferMetal::onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc)
    {
        _commandEncoder.beginRenderPass(_driver->getRenderResourcePool(), frameBufferDesc);
        _splitBarrierTracker.beginEncoder();
        std::vector<id<MTLFence>> fences;
        uint64_t waitStages = 0;
        takePendingWaits(&fences, &waitStages);
        fences.erase(std::remove_if(fences.begin(), fences.end(),
                                    [this](id<MTLFence> fence) {
                                        return !_splitBarrierTracker.checkConsume(
                                            reinterpret_cast<uint64_t>(fence), getDebugName().c_str());
                                    }),
                     fences.end());
        _commandEncoder.waitForProducers(fences, waitStages != 0 ? waitStages
                                                                 : (MTLStageVertex | MTLStageFragment));
        _commandEncoder.flushAliasAcquireIfPending();
    }

    void VkmCommandBufferMetal::onEndRenderPass()
    {
        closeEncoder(MTLStageVertex | MTLStageFragment);
    }

    void VkmCommandBufferMetal::onSetViewportAndScissor(int32_t x, int32_t y, uint32_t width, uint32_t height)
    {
        id<MTL4RenderCommandEncoder> renderCommandEncoder = _commandEncoder.getActiveRenderCommandEncoder();
        VKM_ASSERT(renderCommandEncoder != nil, "setViewportAndScissor outside a render pass");

        const MTLViewport viewport{
            .originX = static_cast<double>(x), .originY = static_cast<double>(y),
            .width = static_cast<double>(width), .height = static_cast<double>(height),
            .znear = 0.0, .zfar = 1.0,
        };
        [renderCommandEncoder setViewport:viewport];
        [renderCommandEncoder setScissorRect:MTLScissorRect{
            static_cast<NSUInteger>(x), static_cast<NSUInteger>(y), width, height}];
    }

    void VkmCommandBufferMetal::onBindPipeline(VkmPipelineStateBase* pipelineState)
    {
        VkmPipelineStateMetal* pipelineStateMetal = static_cast<VkmPipelineStateMetal*>(pipelineState);
        VkmBindlessResourceManagerMetal* bindlessManager =
            static_cast<VkmDriverMetal*>(_driver)->getBindlessResourceManager();

        if (pipelineStateMetal->isCompute())
        {
            /*
            * A compute pipeline bind is what opens the compute pass (and unbindPipeline closes it):
            * a dispatch is recorded outside any render pass, and Metal has no encoder-less compute.
            * One encoder per pass -- never one per barrier, per the MTL4CommandQueueErrorTimeout
            * lesson in common/AGENTS.md.
            */
            if (_commandEncoder.getActiveComputeCommandEncoder() == nullptr)
            {
                openComputeEncoder(MTLStageDispatch);
            }
            id<MTL4ComputeCommandEncoder> computeEncoder = _commandEncoder.getActiveComputeCommandEncoder();
            [computeEncoder setComputePipelineState:pipelineStateMetal->getComputePipelineState()];


            // The compute stage reaches the same engine-global bindless set the graphics stages do.
            id<MTL4ArgumentTable> argumentTable = bindlessManager->getArgumentTable();
            [computeEncoder setArgumentTable:argumentTable];
            [argumentTable setAddress:bindlessManager->getArgumentBuffer().gpuAddress
                              atIndex:kVkmMetalBindlessArgumentBufferIndex];
            // Set 1, on the same terms as the graphics branch below. It was missing here until a
            // compute shader first wanted the camera (the path tracer builds its primary rays from
            // inverseViewProjection), which is the same gap WebGPU had and for the same reason:
            // the scene's cull and emit passes read only set 0, so nothing noticed. Vulkan never
            // had it -- it binds sets 0 and 1 in one call at whichever bind point the pipeline is.
            [argumentTable setAddress:static_cast<VkmDriverMetal*>(_driver)->getFrameConstantManager()->getActiveGpuAddress()
                              atIndex:kVkmMetalFrameConstantBufferIndex];
        }
        else
        {
            id<MTL4RenderCommandEncoder> renderCommandEncoder = _commandEncoder.getActiveRenderCommandEncoder();
            [renderCommandEncoder setRenderPipelineState:pipelineStateMetal->getRenderPipelineState()];
            [renderCommandEncoder setDepthStencilState:pipelineStateMetal->getDepthStencilState()];
            _boundPrimitiveType = static_cast<uint32_t>(
                getPrimitiveType(pipelineStateMetal->getDescriptor().primitiveTopology));

            // Raster state is encoder state on Metal, not part of the pipeline object, so it
            // has to be re-applied per bind -- the same reason the primitive type is resolved
            // here. Vulkan and WebGPU bake all three into their pipeline descriptors.
            const VkmRasterizationStateDescriptor& rasterizationState =
                pipelineStateMetal->getDescriptor().rasterizationState;
            [renderCommandEncoder setTriangleFillMode:getTriangleFillMode(rasterizationState.fillMode)];
            [renderCommandEncoder setCullMode:getCullMode(rasterizationState.cullMode)];
            [renderCommandEncoder setFrontFacingWinding:getFrontFacingWinding(rasterizationState.frontFace)];

            // Every graphics pipeline shares the engine-global bindless argument table:
            // the set-0 argument buffer at [[buffer(2)]] and a safe default for the
            // push-constant slot at [[buffer(3)]] (overwritten per setPushConstants call).
            // Mirrors VkmCommandBufferVulkan::onBindPipeline's implicit set-0 bind.
            id<MTL4ArgumentTable> argumentTable = bindlessManager->getArgumentTable();
            [renderCommandEncoder setArgumentTable:argumentTable
                                          atStages:MTLRenderStageVertex | MTLRenderStageFragment];
            [argumentTable setAddress:bindlessManager->getArgumentBuffer().gpuAddress
                              atIndex:kVkmMetalBindlessArgumentBufferIndex];
            // Set 1: this frame slot's camera constants. Bound for every pipeline, including
            // shaders that never declare them -- an argument-table index no shader reads is
            // inert, and this keeps the bind site free of per-PSO knowledge.
            [argumentTable setAddress:static_cast<VkmDriverMetal*>(_driver)->getFrameConstantManager()->getActiveGpuAddress()
                              atIndex:kVkmMetalFrameConstantBufferIndex];
        }
    }

    void VkmCommandBufferMetal::onUnbindPipeline()
    {
        // No explicit "unbind" concept in Metal for graphics -- the next bindPipeline() call (or the
        // end of the render pass) supersedes whatever pipeline state is set. A compute pass, though,
        // is opened by its pipeline bind, so this is where it closes: publish its writes to
        // everything recorded afterwards, then end the encoder.
        if (_commandEncoder.getActiveComputeCommandEncoder() != nullptr)
        {
            closeEncoder(MTLStageDispatch);
        }
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
        openComputeEncoder(MTLStageBlit);
        id<MTL4ComputeCommandEncoder> computeEncoder = _commandEncoder.getActiveComputeCommandEncoder();
        // Metal4 does no automatic hazard tracking: wait for prior encoders that may have
        // written the source (e.g. render graph capture copying a buffer a pass just used),
        // and make the copy visible to later encoders.
        [computeEncoder copyFromBuffer:mtlSrcBuffer
                          sourceOffset:srcOffset
                              toBuffer:mtlDstBuffer
                     destinationOffset:dstOffset
                                  size:size];
        closeEncoder(MTLStageBlit);
    }

    void VkmCommandBufferMetal::onCopyTexture(VkmResourceHandle srcTexture, VkmResourceHandle dstTexture)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        VkmTextureMetal* srcTextureMetal = static_cast<VkmTextureMetal*>(renderResourcePool->getResource<VkmTexture>(srcTexture));
        VkmTextureMetal* dstTextureMetal = static_cast<VkmTextureMetal*>(renderResourcePool->getResource<VkmTexture>(dstTexture));
        const VkmTextureInfo& srcInfo = srcTextureMetal->getTextureInfo();

        // Metal4 has no blit encoder -- texture copies live on the compute encoder, same
        // per-call encoder tradeoff as onCopyBuffer above (debug-capture path, not per-frame).
        openComputeEncoder(MTLStageBlit);
        id<MTL4ComputeCommandEncoder> computeEncoder = _commandEncoder.getActiveComputeCommandEncoder();
        // Metal4 does no automatic hazard tracking: wait for prior encoders (e.g. the render
        // pass that wrote the source) before copying, and make the copy visible to later
        // encoders (e.g. an ImGui pass sampling the destination).
        [computeEncoder copyFromTexture:srcTextureMetal->getInternalHandle()
                            sourceSlice:0
                            sourceLevel:0
                           sourceOrigin:MTLOriginMake(0, 0, 0)
                             sourceSize:MTLSizeMake(srcInfo._extent.x, srcInfo._extent.y, srcInfo._extent.z)
                              toTexture:dstTextureMetal->getInternalHandle()
                       destinationSlice:0
                       destinationLevel:0
                      destinationOrigin:MTLOriginMake(0, 0, 0)];
        closeEncoder(MTLStageBlit);
    }

    void VkmCommandBufferMetal::onCopyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset, uint32_t arrayLayer)
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
        openComputeEncoder(MTLStageBlit);
        id<MTL4ComputeCommandEncoder> computeEncoder = _commandEncoder.getActiveComputeCommandEncoder();
        [computeEncoder copyFromTexture:textureMetal->getInternalHandle()
                            sourceSlice:arrayLayer
                            sourceLevel:0
                           sourceOrigin:MTLOriginMake(0, 0, 0)
                             sourceSize:MTLSizeMake(textureInfo._extent.x, textureInfo._extent.y, 1)
                               toBuffer:mtlDstBuffer
                      destinationOffset:dstOffset
                 destinationBytesPerRow:bytesPerRow
               destinationBytesPerImage:0];
        closeEncoder(MTLStageBlit);
    }

    void VkmCommandBufferMetal::onCopyBufferToTexture(VkmResourceHandle srcBuffer, VkmResourceHandle dstTexture,
                                                      uint64_t srcOffset, uint32_t mipLevel, uint32_t arrayLayer)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        VkmTextureMetal* textureMetal = static_cast<VkmTextureMetal*>(renderResourcePool->getResource<VkmTexture>(dstTexture));
        id<MTLBuffer> mtlSrcBuffer = resolveMTLBuffer(renderResourcePool, srcBuffer);

        const VkmTextureInfo& textureInfo = textureMetal->getTextureInfo();
        const uint32_t mipWidth = std::max(1u, textureInfo._extent.x >> mipLevel);
        const uint32_t mipHeight = std::max(1u, textureInfo._extent.y >> mipLevel);
        const uint32_t bytesPerRow = mipWidth * vkmBytesPerTexel(textureInfo._format);

        // Metal4 has no blit encoder -- texture copies live on the compute encoder, same
        // per-call encoder rationale as onCopyBuffer above (setup-time upload, not per-frame).
        // Metal tracks no image layouts, so unlike Vulkan there is nothing to transition:
        // the barriers alone make the written texels visible to later encoders' sampling.
        openComputeEncoder(MTLStageBlit);
        id<MTL4ComputeCommandEncoder> computeEncoder = _commandEncoder.getActiveComputeCommandEncoder();
        [computeEncoder copyFromBuffer:mtlSrcBuffer
                          sourceOffset:srcOffset
                     sourceBytesPerRow:bytesPerRow
                   sourceBytesPerImage:static_cast<NSUInteger>(bytesPerRow) * mipHeight
                            sourceSize:MTLSizeMake(mipWidth, mipHeight, 1)
                             toTexture:textureMetal->getInternalHandle()
                      destinationSlice:arrayLayer
                      destinationLevel:mipLevel
                     destinationOrigin:MTLOriginMake(0, 0, 0)];
        closeEncoder(MTLStageBlit);
    }

    void VkmCommandBufferMetal::onDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        [_commandEncoder.getActiveRenderCommandEncoder() drawPrimitives:static_cast<MTLPrimitiveType>(_boundPrimitiveType)
                                                            vertexStart:firstVertex
                                                            vertexCount:vertexCount
                                                          instanceCount:instanceCount
                                                           baseInstance:firstInstance];
    }

    void VkmCommandBufferMetal::onDrawIndirectCount(VkmIndirectArgumentLayout layout,
                                                    VkmResourceHandle argumentBuffer, uint64_t argumentOffset,
                                                    VkmResourceHandle countBuffer, uint64_t countOffset,
                                                    uint32_t maxDrawCount)
    {
        // Metal 4 has no multi-draw and no GPU-side draw count for a plain indirect draw, so this
        // encodes one indirect draw per candidate slot. All-zero argument records draw nothing,
        // which is what culled slots are -- see VkmCommandBufferBase::drawIndirectCount for the
        // compaction contract that makes this agree with Vulkan's vkCmdDrawIndirectCount.
        (void)countBuffer;
        (void)countOffset;

        const uint64_t argumentStride = vkmGetIndirectArgumentStride(layout);
        id<MTLBuffer> mtlArgumentBuffer = resolveMTLBuffer(_driver->getRenderResourcePool(), argumentBuffer);
        const MTLGPUAddress base = mtlArgumentBuffer.gpuAddress + argumentOffset;
        id<MTL4RenderCommandEncoder> renderCommandEncoder = _commandEncoder.getActiveRenderCommandEncoder();
        for (uint32_t i = 0; i < maxDrawCount; ++i)
        {
            [renderCommandEncoder drawPrimitives:static_cast<MTLPrimitiveType>(_boundPrimitiveType)
                                 indirectBuffer:base + static_cast<uint64_t>(i) * argumentStride];
        }
    }

    void VkmCommandBufferMetal::onDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
    {
        // MTLComputePipelineState cannot be asked what [numthreads(...)] its function declared,
        // so the size travels from the shader through its .vfcache onto the pipeline object.
        // Dispatching with the wrong one silently runs the wrong number of threads, which is why
        // this reads the bound pipeline rather than assuming one engine-wide size.
        const VkmPipelineStateMetal* pipelineState =
            static_cast<const VkmPipelineStateMetal*>(getBoundPipelineState());
        if (pipelineState == nullptr)
        {
            VKM_DEBUG_ERROR("dispatch() with no pipeline bound");
            return;
        }

        const uint32_t* threadGroupSize = pipelineState->getComputeThreadGroupSize();
        if (threadGroupSize[0] == 0)
        {
            VKM_DEBUG_ERROR("dispatch() on a pipeline with no compute threadgroup size (not a "
                            "compute pipeline, or a stale shader cache)");
            return;
        }

        [_commandEncoder.getActiveComputeCommandEncoder()
            dispatchThreadgroups:MTLSizeMake(groupCountX, groupCountY, groupCountZ)
           threadsPerThreadgroup:MTLSizeMake(threadGroupSize[0], threadGroupSize[1], threadGroupSize[2])];
    }

    void VkmCommandBufferMetal::onBuildAccelerationStructure(VkmResourceHandle accelerationStructure)
    {
        VkmAccelerationStructure* structure =
            _driver->getRenderResourcePool()->getResource<VkmAccelerationStructure>(accelerationStructure);
        if (structure == nullptr)
        {
            VKM_DEBUG_ERROR("buildAccelerationStructure: invalid acceleration structure handle");
            return;
        }

        // Opened here rather than reusing an active encoder: a build is recorded outside a render
        // pass, and the encoder is closed immediately so the barrier pair onUnbindPipeline emits
        // for compute passes also publishes this build to whatever traverses it afterwards.
        if (_commandEncoder.getActiveComputeCommandEncoder() == nullptr)
        {
            openComputeEncoder(MTLStageBlit);
        }
        id<MTL4ComputeCommandEncoder> encoder = _commandEncoder.getActiveComputeCommandEncoder();
        VkmAccelerationStructureMetal* structureMetal = static_cast<VkmAccelerationStructureMetal*>(structure);
        if (structureMetal->getAccelerationStructure() == nil || structureMetal->getDescriptor() == nil)
        {
            VKM_DEBUG_ERROR("buildAccelerationStructure on a structure that failed to initialize");
            closeEncoder(MTLStageBlit);
            return;
        }
        id<MTLBuffer> scratch = structureMetal->getScratchBuffer();
        if (scratch == nil)
        {
            // The scratch is gone on a static structure, which is exactly the case this must
            // refuse: rebuilding one would read whatever now occupies that memory. Passing an
            // empty range instead is not a softer failure -- Metal's debug layer aborts the
            // process on a nil scratch buffer, which is how this guard's absence surfaced.
            VKM_DEBUG_ERROR("buildAccelerationStructure on a structure that was not created with _allowUpdate");
            closeEncoder(MTLStageBlit);
            return;
        }
        [encoder buildAccelerationStructure:structureMetal->getAccelerationStructure()
                                 descriptor:structureMetal->getDescriptor()
                              scratchBuffer:MTL4BufferRange{ scratch.gpuAddress, scratch.length }];
        closeEncoder(MTLStageBlit);
    }

    void VkmCommandBufferMetal::openComputeEncoder(uint64_t waitBeforeStages)
    {
        _commandEncoder.beginComputePass();
        _splitBarrierTracker.beginEncoder();
        std::vector<id<MTLFence>> fences;
        uint64_t stages = 0;
        takePendingWaits(&fences, &stages);
        fences.erase(std::remove_if(fences.begin(), fences.end(),
                                    [this](id<MTLFence> fence) {
                                        return !_splitBarrierTracker.checkConsume(
                                            reinterpret_cast<uint64_t>(fence), getDebugName().c_str());
                                    }),
                     fences.end());
        _commandEncoder.waitForProducers(fences, stages != 0 ? stages : waitBeforeStages);
        _commandEncoder.flushAliasAcquireIfPending();
    }

    void VkmCommandBufferMetal::closeEncoder(uint64_t publishAfterStages)
    {
        // Both halves record only what the encoder actually encoded: a fence marked as produced
        // when no update reached the encoder is one a later wait would block on forever.
        if (_commandEncoder.publishThroughFence(_currentSubGraphFence, publishAfterStages))
        {
            _publishedSubGraphs.insert(_currentSubGraphId);
            _splitBarrierTracker.recordProduce(reinterpret_cast<uint64_t>(_currentSubGraphFence),
                                               ("SubGraph#" + std::to_string(_currentSubGraphId)).c_str());
        }
        id<MTLFence> boundaryFence = fenceForEncoderBoundary();
        if (_commandEncoder.publishThroughFence(boundaryFence, publishAfterStages))
        {
            _encoderBoundaryPublished = true;
            _splitBarrierTracker.recordProduce(reinterpret_cast<uint64_t>(boundaryFence),
                                               "encoder boundary");
        }
        _commandEncoder.commit();
    }

    id<MTLFence> VkmCommandBufferMetal::fenceForSubGraph(uint32_t subGraphId)
    {
        const auto it = _subGraphFences.find(subGraphId);
        if (it != _subGraphFences.end())
        {
            return it->second;
        }
        id<MTLFence> fence = [static_cast<VkmDriverMetal*>(_driver)->getMTLDevice() newFence];
        if (fence == nil)
        {
            VKM_DEBUG_ERROR("Failed to create an MTLFence for a render subgraph");
            return nil;
        }
        _subGraphFences.emplace(subGraphId, fence);
        return fence;
    }

    void VkmCommandBufferMetal::onAcquireAliasedTexture(VkmResourceHandle texture)
    {
        (void)texture;
        /*
        * Records nothing here on purpose. Metal has no image layouts, so there is no discard to
        * issue -- but the aliasing hazard is real, and a barrier needs an encoder to ride:
        * opening one just for this is what caused the MTL4CommandQueueErrorTimeout in
        * common/AGENTS.md, and a barrier in a subgraph that binds no pipeline records nothing.
        *
        * So the next encoder this command buffer opens carries it. What the ordinary barrier
        * path lacks is MTL4VisibilityOptionResourceAlias, which flushes the caches that make two
        * virtual addresses over the same bytes coherent, and this latch is what adds it.
        */
        _commandEncoder.markAliasAcquirePending();
    }

    id<MTLFence> VkmCommandBufferMetal::fenceForEncoderBoundary()
    {
        if (_encoderBoundaryFence == nil)
        {
            _encoderBoundaryFence = [static_cast<VkmDriverMetal*>(_driver)->getMTLDevice() newFence];
            if (_encoderBoundaryFence == nil)
            {
                VKM_DEBUG_ERROR("Failed to create an MTLFence for the encoder boundary");
            }
        }
        return _encoderBoundaryFence;
    }

    void VkmCommandBufferMetal::releaseFences()
    {
        // These sources are compiled without ARC, so a fence held across frames is owned here.
        for (auto& entry : _subGraphFences)
        {
            [entry.second release];
        }
        _subGraphFences.clear();
        [_encoderBoundaryFence release];
        _encoderBoundaryFence = nil;
    }

    void VkmCommandBufferMetal::onBarrierAcquire(const VkmResourceBarrier* barriers, uint32_t count)
    {
        /*
        * Called before a subgraph commits, so no encoder is open yet and the waits are accumulated
        * for whichever one opens first. A dependency whose producer lies outside this graph has no
        * fence to wait on: cross-submit ordering is the queue's own timeline, and a first touch
        * within a frame has nothing before it to wait for.
        */
        for (uint32_t i = 0; i < count; ++i)
        {
            if (barriers[i]._producerSubGraphId == INVALID_VALUE32)
            {
                continue;
            }
            if (_publishedSubGraphs.find(barriers[i]._producerSubGraphId) == _publishedSubGraphs.end())
            {
                // That producer recorded no GPU work, so its fence is never updated and waiting on
                // it would hang the queue. Nothing was written, so there is nothing to wait for.
                continue;
            }
            id<MTLFence> fence = fenceForSubGraph(barriers[i]._producerSubGraphId);
            if (fence != nil &&
                std::find(_pendingWaitFences.begin(), _pendingWaitFences.end(), fence) == _pendingWaitFences.end())
            {
                _pendingWaitFences.push_back(fence);
            }
            _pendingWaitBeforeStages |= vkmToMTLStages(barriers[i]._dstAccess, barriers[i]._dstScope);
        }
    }

    void VkmCommandBufferMetal::onBarrierRelease(const VkmResourceBarrier* barriers, uint32_t count)
    {
        /*
        * What the subgraph about to run publishes. Recorded before it commits, because the update
        * has to sit inside that subgraph's encoders and they are closed by the time commit()
        * returns. Replaced rather than accumulated: this names one subgraph's fence, and carrying
        * the previous one forward would publish through the wrong fence.
        */
        _currentSubGraphFence = nullptr;
        _currentSubGraphId = INVALID_VALUE32;
        _currentSubGraphReleaseStages = 0;
        if (count == 0)
        {
            return;
        }
        // Every entry in a release list shares one producer -- this subgraph.
        _currentSubGraphId = barriers[0]._producerSubGraphId;
        _currentSubGraphFence = fenceForSubGraph(_currentSubGraphId);
        for (uint32_t i = 0; i < count; ++i)
        {
            _currentSubGraphReleaseStages |= vkmToMTLStages(barriers[i]._srcAccess, barriers[i]._srcScope);
        }
    }

    void VkmCommandBufferMetal::onResourceBarrier(const VkmResourceBarrier* barriers, uint32_t count)
    {
        MTLStages afterStages = 0;
        MTLStages beforeStages = 0;
        bool anyWrite = false;
        for (uint32_t i = 0; i < count; ++i)
        {
            afterStages |= vkmToMTLStages(barriers[i]._srcAccess, barriers[i]._srcScope);
            beforeStages |= vkmToMTLStages(barriers[i]._dstAccess, barriers[i]._dstScope);
            anyWrite = anyWrite || !barriers[i]._executionOnly;
        }
        // A pure write-after-read needs the two sides ordered but no cache flush, since a read
        // publishes nothing.
        const MTL4VisibilityOptions visibility =
            anyWrite ? MTL4VisibilityOptionDevice : MTL4VisibilityOptionNone;

        /*
        * Ride whichever encoder is already open. Opening one for a barrier is what caused the
        * MTL4CommandQueueErrorTimeout this backend is built to avoid, so the alternative when
        * nothing is open is to hand the ordering to the next encoder that opens, not to open one.
        */
        id<MTL4ComputeCommandEncoder> computeEncoder = _commandEncoder.getActiveComputeCommandEncoder();
        if (computeEncoder != nil)
        {
            [computeEncoder barrierAfterEncoderStages:(afterStages & kComputeEncodableStages) ?: MTLStageDispatch
                                  beforeEncoderStages:(beforeStages & kComputeEncodableStages) ?: MTLStageDispatch
                                    visibilityOptions:visibility];
            return;
        }

        id<MTL4RenderCommandEncoder> renderEncoder = _commandEncoder.getActiveRenderCommandEncoder();
        if (renderEncoder != nil)
        {
            [renderEncoder barrierAfterEncoderStages:(afterStages & kRenderEncodableStages) ?: MTLStageFragment
                                 beforeEncoderStages:(beforeStages & kRenderEncodableStages) ?: MTLStageVertex
                                   visibilityOptions:visibility];
            return;
        }

        /*
        * Nothing is open, so the two sides sit in different encoders and the ordering belongs to
        * whichever one opens next -- opening one here is what causes MTL4CommandQueueErrorTimeout.
        * The producing side is already closed and updated the boundary fence on its way out, so the
        * wait has something to pair with. Without it the two encoders run unordered and the reader
        * sees a partially written resource, which Metal reports as neither an error nor a hang.
        */
        if (_encoderBoundaryPublished)
        {
            id<MTLFence> boundaryFence = fenceForEncoderBoundary();
            if (boundaryFence != nil &&
                std::find(_pendingWaitFences.begin(), _pendingWaitFences.end(), boundaryFence) ==
                    _pendingWaitFences.end())
            {
                _pendingWaitFences.push_back(boundaryFence);
            }
        }
        _pendingWaitBeforeStages |= beforeStages;
    }

    void VkmCommandBufferMetal::onBindResourceTable(VkmResourceTableBase* table)
    {
        // Metal binds set 2 discretely onto the same argument table bindPipeline() already
        // attached, so this only has to replay the table's resolved entries -- there is no
        // descriptor set to bind, and no encoder state beyond the argument table itself.
        VkmBindlessResourceManagerMetal* bindlessManager =
            static_cast<VkmDriverMetal*>(_driver)->getBindlessResourceManager();
        static_cast<VkmResourceTableMetal*>(table)->applyTo(bindlessManager->getArgumentTable());
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

    void VkmCommandBufferMetal::onBeginCommandBuffer()
    {
        _openGpuZoneEndSlots.clear();
        // Command buffers are pooled, so ordering staged for a previous frame's last subgraph must
        // not reach this one's first encoder.
        _pendingWaitFences.clear();
        _pendingWaitBeforeStages = 0;
        _currentSubGraphFence = nullptr;
        _currentSubGraphId = INVALID_VALUE32;
        _currentSubGraphReleaseStages = 0;
        _publishedSubGraphs.clear();
        _encoderBoundaryPublished = false;
        _splitBarrierTracker.reset();
    }

    void VkmCommandBufferMetal::onBeginGpuZone(const uint32_t beginSlot, const uint32_t endSlot)
    {
        id<MTL4CounterHeap> counterHeap = static_cast<VkmDriverMetal*>(_driver)->getGpuTimestampCounterHeap();
        if (counterHeap == nil)
        {
            return;
        }

        // Command-buffer scope, like the debug group above: the encoders are nil between passes,
        // and this is called from VkmRenderGraph::execute() outside any of them. That is exactly
        // what lets a zone wrap a whole subgraph without splitting an encoder -- the per-encoder
        // writeTimestampWithGranularity: variants would.
        [_mtlCommandBuffer writeTimestampIntoHeap:counterHeap atIndex:beginSlot];
        _openGpuZoneEndSlots.push_back(endSlot);
    }

    bool VkmCommandBufferMetal::onEndGpuZone()
    {
        id<MTL4CounterHeap> counterHeap = static_cast<VkmDriverMetal*>(_driver)->getGpuTimestampCounterHeap();
        if (counterHeap == nil || _openGpuZoneEndSlots.empty())
        {
            return false;
        }

        [_mtlCommandBuffer writeTimestampIntoHeap:counterHeap atIndex:_openGpuZoneEndSlots.back()];
        _openGpuZoneEndSlots.pop_back();
        return true;
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
        openComputeEncoder(MTLStageBlit);
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
        closeEncoder(MTLStageBlit);

        _pendingMarkerWrites.clear();
    }
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS
}
