// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_command_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_texture.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>
#include <vkm/renderer/backend/webgpu/webgpu_pipeline_state.h>
#include <vkm/renderer/backend/webgpu/webgpu_resource_table.h>
#include <vkm/renderer/backend/webgpu/webgpu_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_staging_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/render_pass.h>

#include <vector>

namespace vkm
{
    VkmCommandBufferWebGPU::VkmCommandBufferWebGPU(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool)
        : VkmCommandBufferBase(driver, commandQueue, commandBufferPool)
    {
    }

    VkmCommandBufferWebGPU::~VkmCommandBufferWebGPU()
    {
    }

    void VkmCommandBufferWebGPU::setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle)
    {
        _encoder = static_cast<WGPUCommandEncoder>(handle);
    }

    void VkmCommandBufferWebGPU::onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();

        std::vector<WGPUTextureView> colorViews;
        colorViews.reserve(frameBufferDesc._renderPass._colorAttachmentCount);

        std::vector<WGPURenderPassColorAttachment> colorAttachments;
        colorAttachments.reserve(frameBufferDesc._renderPass._colorAttachmentCount);
        for (uint32_t i = 0; i < frameBufferDesc._renderPass._colorAttachmentCount; ++i)
        {
            const VkmColorAttachmentDescriptor& colorDesc = frameBufferDesc._renderPass._colorAttachments[i];
            VkmTextureWebGPU* texture = static_cast<VkmTextureWebGPU*>(renderResourcePool->getResource<VkmTexture>(frameBufferDesc._colorAttachments[i]));

            WGPUTextureView view = wgpuTextureCreateView(texture->getWGPUTexture(), nullptr);
            colorViews.push_back(view);

            colorAttachments.push_back(WGPURenderPassColorAttachment{
                .view       = view,
                // Zero-initialization would mean "3D texture slice 0", which WebGPU
                // rejects for 2D attachments -- must be explicitly undefined.
                .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
                .loadOp     = toWGPULoadOp(colorDesc._loadAction),
                .storeOp    = toWGPUStoreOp(colorDesc._storeAction),
                .clearValue = WGPUColor{colorDesc._clearColors[0], colorDesc._clearColors[1], colorDesc._clearColors[2], colorDesc._clearColors[3]},
            });
        }

        WGPURenderPassDepthStencilAttachment depthStencilAttachment{};
        WGPUTextureView depthStencilView = nullptr;
        if (frameBufferDesc._depthStencilAttachment.has_value() &&
            frameBufferDesc._renderPass._depthStencilAttachment.has_value())
        {
            VkmTextureWebGPU* depthTexture = static_cast<VkmTextureWebGPU*>(
                renderResourcePool->getResource<VkmTexture>(frameBufferDesc._depthStencilAttachment.value()));
            const VkmDepthStencilAttachmentDescriptor& depthDesc =
                frameBufferDesc._renderPass._depthStencilAttachment.value();
            const VkmFormat depthFormat = depthTexture->getTextureInfo()._format;

            depthStencilView = wgpuTextureCreateView(depthTexture->getWGPUTexture(), nullptr);
            depthStencilAttachment.view = depthStencilView;

            // WebGPU rejects load/store ops for an aspect the attachment format doesn't
            // have, so each aspect is only configured when the format actually carries it.
            if (hasDepth(depthFormat))
            {
                depthStencilAttachment.depthLoadOp = toWGPULoadOp(depthDesc._loadAction);
                depthStencilAttachment.depthStoreOp = toWGPUStoreOp(depthDesc._storeAction);
                depthStencilAttachment.depthClearValue = depthDesc._clearDepth;
            }
            if (hasStencil(depthFormat))
            {
                depthStencilAttachment.stencilLoadOp = toWGPULoadOp(depthDesc._loadAction);
                depthStencilAttachment.stencilStoreOp = toWGPUStoreOp(depthDesc._storeAction);
                depthStencilAttachment.stencilClearValue = depthDesc._clearStencil;
            }
        }

        WGPUPassTimestampWrites timestampWrites{};
        const bool hasTimestampWrites = takePendingGpuZone(&timestampWrites);

        const WGPURenderPassDescriptor renderPassDesc{
            .colorAttachmentCount   = colorAttachments.size(),
            .colorAttachments       = colorAttachments.data(),
            .depthStencilAttachment = depthStencilView != nullptr ? &depthStencilAttachment : nullptr,
            .timestampWrites        = hasTimestampWrites ? &timestampWrites : nullptr,
        };
        _renderPassEncoder = wgpuCommandEncoderBeginRenderPass(_encoder, &renderPassDesc);

        // wgpuCommandEncoderBeginRenderPass only needs the views for the duration of this call.
        for (WGPUTextureView view : colorViews)
        {
            wgpuTextureViewRelease(view);
        }
        if (depthStencilView != nullptr)
        {
            wgpuTextureViewRelease(depthStencilView);
        }
    }

    void VkmCommandBufferWebGPU::onEndRenderPass()
    {
        wgpuRenderPassEncoderEnd(_renderPassEncoder);
        wgpuRenderPassEncoderRelease(_renderPassEncoder);
        _renderPassEncoder = nullptr;
    }

    void VkmCommandBufferWebGPU::onSetViewportAndScissor(int32_t x, int32_t y, uint32_t width, uint32_t height)
    {
        VKM_ASSERT(_renderPassEncoder != nullptr, "setViewportAndScissor outside a render pass");
        wgpuRenderPassEncoderSetViewport(_renderPassEncoder, static_cast<float>(x), static_cast<float>(y),
                                         static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
        wgpuRenderPassEncoderSetScissorRect(_renderPassEncoder, static_cast<uint32_t>(x),
                                            static_cast<uint32_t>(y), width, height);
    }

    void VkmCommandBufferWebGPU::onBindPipeline(VkmPipelineStateBase* pipelineState)
    {
        VkmPipelineStateWebGPU* pipelineStateWebGPU = static_cast<VkmPipelineStateWebGPU*>(pipelineState);
        if (pipelineStateWebGPU->isCompute())
        {
            // Mirrors Metal's compute lifecycle (see VkmCommandEncoderMetal::beginComputePass):
            // there is no separate begin/end-compute entry point on the common command-buffer
            // interface, so the compute pass encoder is lazily created here on first bind and
            // torn down in onUnbindPipeline.
            if (_computePassEncoder == nullptr)
            {
                WGPUPassTimestampWrites timestampWrites{};
                const WGPUComputePassDescriptor computePassDesc{
                    .timestampWrites = takePendingGpuZone(&timestampWrites) ? &timestampWrites : nullptr,
                };
                _computePassEncoder = wgpuCommandEncoderBeginComputePass(_encoder, &computePassDesc);
            }
            wgpuComputePassEncoderSetPipeline(_computePassEncoder, pipelineStateWebGPU->getComputePipeline());

            // The compute stage reaches the same engine-global bind group 0 the graphics stages do,
            // with a zero dynamic offset as a safe default (overwritten per setPushConstants call).
            VkmBindlessResourceManagerWebGPU* computeBindlessManager =
                static_cast<VkmDriverWebGPU*>(_driver)->getBindlessResourceManager();
            const uint32_t computeZeroOffset = 0;
            wgpuComputePassEncoderSetBindGroup(_computePassEncoder, 0, computeBindlessManager->getBindGroup(true),
                                               1, &computeZeroOffset);
            // And group 1, for the same reason the graphics path below does it: WebGPU requires
            // every group the pipeline layout declares to be set before a dispatch, referenced by
            // the shader or not. Leaving it out costs "No bind group set at group index 1" and the
            // whole command buffer.
            wgpuComputePassEncoderSetBindGroup(_computePassEncoder, 1,
                                               static_cast<VkmDriverWebGPU*>(_driver)
                                                   ->getFrameConstantManager()->getActiveBindGroup(),
                                               0, nullptr);
            return;
        }

        VKM_ASSERT(_renderPassEncoder != nullptr, "onBindPipeline called for a graphics pipeline outside an active render pass");
        wgpuRenderPassEncoderSetPipeline(_renderPassEncoder, pipelineStateWebGPU->getRenderPipeline());

        // Every graphics pipeline shares the engine-global bindless bind group 0; bind it
        // with a zero dynamic offset as a safe default so draws that never push constants
        // are still valid (mirrors VkmCommandBufferVulkan::onBindPipeline's set-0 bind).
        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);
        const uint32_t zeroOffset = 0;
        wgpuRenderPassEncoderSetBindGroup(_renderPassEncoder, 0,
                                          driverWebGPU->getBindlessResourceManager()->getBindGroup(false),
                                          1, &zeroOffset);
        // Group 1 is this frame slot's camera constants. Set unconditionally: WebGPU requires
        // every group the pipeline layout declares to be set before a draw, whether or not the
        // shader references it.
        wgpuRenderPassEncoderSetBindGroup(_renderPassEncoder, 1,
                                          driverWebGPU->getFrameConstantManager()->getActiveBindGroup(),
                                          0, nullptr);
    }

    void VkmCommandBufferWebGPU::onUnbindPipeline()
    {
        if (_computePassEncoder != nullptr)
        {
            wgpuComputePassEncoderEnd(_computePassEncoder);
            wgpuComputePassEncoderRelease(_computePassEncoder);
            _computePassEncoder = nullptr;
        }
        // WebGPU has no explicit "unbind pipeline" call for a graphics pipeline -- render-pass
        // pipeline state is simply replaced by the next bind or discarded at pass end.
    }

    // copyBuffer() accepts either a Buffer or a StagingBuffer resource on either side (see
    // the same-named helper in vulkan_command_buffer.cpp) -- resolve via the handle's own
    // recorded type. WebGPU buffers are always dedicated WGPUBuffers, no base offset.
    static WGPUBuffer resolveWGPUBuffer(VkmRenderResourcePool* renderResourcePool, VkmResourceHandle handle)
    {
        if (handle.type == VkmResourceType::StagingBuffer)
        {
            return static_cast<VkmStagingBufferWebGPU*>(renderResourcePool->getResource<VkmStagingBuffer>(handle))->getBuffer();
        }
        return static_cast<VkmBufferWebGPU*>(renderResourcePool->getResource<VkmBuffer>(handle))->getBuffer();
    }

    void VkmCommandBufferWebGPU::onCopyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size)
    {
        // WebGPU requires 4-byte-aligned buffer-to-buffer copy offsets and size.
        VKM_ASSERT((srcOffset % 4 == 0) && (dstOffset % 4 == 0) && (size % 4 == 0),
            "WebGPU buffer copies require 4-byte-aligned offsets and size");

        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        wgpuCommandEncoderCopyBufferToBuffer(_encoder,
                                             resolveWGPUBuffer(renderResourcePool, srcBuffer), srcOffset,
                                             resolveWGPUBuffer(renderResourcePool, dstBuffer), dstOffset,
                                             size);
    }

    void VkmCommandBufferWebGPU::onCopyTexture(VkmResourceHandle srcTexture, VkmResourceHandle dstTexture)
    {
        // Texture-to-texture copies (render graph capture snapshots) are Metal-only for
        // now -- see VkmDriverCapabilityFlags::TextureContentCapture.
        VKM_DEBUG_ERROR("copyTexture is not implemented on the WebGPU backend");
    }

    void VkmCommandBufferWebGPU::onCopyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset, uint32_t arrayLayer)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        VkmTextureWebGPU* textureWebGPU = static_cast<VkmTextureWebGPU*>(renderResourcePool->getResource<VkmTexture>(srcTexture));

        const VkmTextureInfo& textureInfo = textureWebGPU->getTextureInfo();
        const uint32_t bytesPerRow = textureInfo._extent.x * vkmBytesPerTexel(textureInfo._format);
        // WebGPU requires texture->buffer copy rows to be 256-byte aligned.
        VKM_ASSERT(bytesPerRow % 256 == 0, "WebGPU texture readback requires a 256-byte-aligned row pitch");

        WGPUTexelCopyTextureInfo source{};
        source.texture = textureWebGPU->getWGPUTexture();
        source.mipLevel = 0;
        source.origin.z = arrayLayer;
        source.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferInfo destination{};
        destination.layout.offset = dstOffset;
        destination.layout.bytesPerRow = bytesPerRow;
        destination.layout.rowsPerImage = textureInfo._extent.y;
        destination.buffer = resolveWGPUBuffer(renderResourcePool, dstBuffer);

        const WGPUExtent3D copySize{textureInfo._extent.x, textureInfo._extent.y, 1};
        wgpuCommandEncoderCopyTextureToBuffer(_encoder, &source, &destination, &copySize);
    }

    void VkmCommandBufferWebGPU::onCopyBufferToTexture(VkmResourceHandle srcBuffer, VkmResourceHandle dstTexture,
                                                       uint64_t srcOffset, uint32_t mipLevel, uint32_t arrayLayer)
    {
        // Uploading the pixels would work here, but nothing could sample them: WGSL has no
        // runtime-sized texture arrays, so this backend's bindless layer (mega-buffer
        // emulation) models no texture array to register the result into. Left unimplemented
        // as a pair with registerTexture -- see VkmDriverCapabilityFlags::TextureUpload.
        VKM_DEBUG_ERROR("copyBufferToTexture is not implemented on the WebGPU backend");
    }

    void VkmCommandBufferWebGPU::onDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        wgpuRenderPassEncoderDraw(_renderPassEncoder, vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void VkmCommandBufferWebGPU::onSetPushConstants(const void* data, uint32_t size, uint32_t offset)
    {
        // Each call fills a fresh ring entry, so a partial update at a non-zero offset
        // cannot preserve previously-pushed bytes; no caller needs that today.
        VKM_ASSERT(offset == 0, "WebGPU push-constant emulation only supports offset 0");

        VkmBindlessResourceManagerWebGPU* bindlessManager =
            static_cast<VkmDriverWebGPU*>(_driver)->getBindlessResourceManager();
        const uint32_t dynamicOffset = bindlessManager->writePushConstants(data, size);
        // A compute pass has its own encoder; the render-pass encoder is null there.
        if (_computePassEncoder != nullptr)
        {
            wgpuComputePassEncoderSetBindGroup(_computePassEncoder, 0, bindlessManager->getBindGroup(true), 1, &dynamicOffset);
            return;
        }
        wgpuRenderPassEncoderSetBindGroup(_renderPassEncoder, 0, bindlessManager->getBindGroup(false), 1, &dynamicOffset);
    }

    void VkmCommandBufferWebGPU::onDrawIndirectCount(VkmIndirectArgumentLayout layout,
                                                     VkmResourceHandle argumentBuffer, uint64_t argumentOffset,
                                                     VkmResourceHandle countBuffer, uint64_t countOffset,
                                                     uint32_t maxDrawCount)
    {
        // Core WebGPU's drawIndirect issues exactly one draw and has no GPU-side count, so this
        // encodes one per candidate slot and relies on all-zero argument records being no-ops --
        // see VkmCommandBufferBase::drawIndirectCount for the compaction contract. The worst case
        // therefore matches a plain draw per object; it is never worse.
        (void)countBuffer;
        (void)countOffset;

        const uint64_t argumentStride = vkmGetIndirectArgumentStride(layout);
        WGPUBuffer wgpuArgumentBuffer = resolveWGPUBuffer(_driver->getRenderResourcePool(), argumentBuffer);
        for (uint32_t i = 0; i < maxDrawCount; ++i)
        {
            wgpuRenderPassEncoderDrawIndirect(_renderPassEncoder, wgpuArgumentBuffer,
                                             argumentOffset + static_cast<uint64_t>(i) * argumentStride);
        }
    }

    void VkmCommandBufferWebGPU::onDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
    {
        wgpuComputePassEncoderDispatchWorkgroups(_computePassEncoder, groupCountX, groupCountY, groupCountZ);
    }

    void VkmCommandBufferWebGPU::onBuildAccelerationStructure(VkmResourceHandle accelerationStructure)
    {
        (void)accelerationStructure;
        // WebGPU has no acceleration structure in the API, so there is nothing to rebuild and nothing that could have created one.
        VKM_DEBUG_ERROR("buildAccelerationStructure is not supported on the WebGPU backend");
    }

    void VkmCommandBufferWebGPU::onResourceBarrier(const VkmResourceBarrier* barriers, uint32_t count)
    {
        // WebGPU is implicitly synchronised: writes from one pass are visible to the next by
        // specification, so there is nothing to record. The ordering only holds across *pass*
        // boundaries, which the engine's one-subgraph-per-pass convention guarantees.
        (void)barriers;
        (void)count;
    }

    void VkmCommandBufferWebGPU::onBarrierTextureForShaderRead(VkmResourceHandle texture)
    {
        (void)texture;
        // WebGPU has no explicit barriers and no
        // image layouts, and one pass's writes are visible to the next by specification.
    }

    void VkmCommandBufferWebGPU::onBindResourceTable(VkmResourceTableBase* table)
    {
        // Group 2 carries no dynamic offset, unlike group 0's push-constant ring, so this is a
        // plain bind on whichever encoder the bound pipeline opened.
        WGPUBindGroup bindGroup = static_cast<VkmResourceTableWebGPU*>(table)->getBindGroup();
        if (_computePassEncoder != nullptr)
        {
            wgpuComputePassEncoderSetBindGroup(_computePassEncoder, table->getSetIndex(), bindGroup, 0, nullptr);
            return;
        }
        VKM_ASSERT(_renderPassEncoder != nullptr, "bindResourceTable outside any pass");
        wgpuRenderPassEncoderSetBindGroup(_renderPassEncoder, table->getSetIndex(), bindGroup, 0, nullptr);
    }

    void VkmCommandBufferWebGPU::onSetDebugName(const char* name)
    {
        wgpuCommandEncoderSetLabel(_encoder, toWGPUStringView(name));
    }

    void VkmCommandBufferWebGPU::onPushDebugGroup(const char* name)
    {
        // Command-encoder scope fully contains each subgraph's render pass (the wrap site is
        // outside the pass), satisfying WebGPU's debug-group nesting rules.
        wgpuCommandEncoderPushDebugGroup(_encoder, toWGPUStringView(name));
    }

    void VkmCommandBufferWebGPU::onPopDebugGroup()
    {
        wgpuCommandEncoderPopDebugGroup(_encoder);
    }

    void VkmCommandBufferWebGPU::onBeginCommandBuffer()
    {
        _openGpuZones.clear();
    }

    void VkmCommandBufferWebGPU::onBeginGpuZone(const uint32_t beginSlot, const uint32_t endSlot)
    {
        // Nothing can be recorded yet: WebGPU writes timestamps only through a pass descriptor,
        // so the pair waits here until a render or compute pass is opened inside this zone.
        _openGpuZones.push_back(OpenGpuZone{ beginSlot, endSlot, false });
    }

    bool VkmCommandBufferWebGPU::onEndGpuZone()
    {
        if (_openGpuZones.empty())
        {
            return false;
        }
        const bool attached = _openGpuZones.back()._attached;
        _openGpuZones.pop_back();
        return attached;
    }

    bool VkmCommandBufferWebGPU::takePendingGpuZone(WGPUPassTimestampWrites* outTimestampWrites)
    {
        WGPUQuerySet querySet = static_cast<VkmDriverWebGPU*>(_driver)->getGpuTimestampQuerySet();
        if (querySet == nullptr)
        {
            return false;
        }

        // Innermost first: a subgraph's zone is opened inside the submission-wide one, and it is
        // the subgraph that this pass actually belongs to.
        for (auto zone = _openGpuZones.rbegin(); zone != _openGpuZones.rend(); ++zone)
        {
            if (zone->_attached)
            {
                continue;
            }
            zone->_attached = true;
            outTimestampWrites->querySet = querySet;
            outTimestampWrites->beginningOfPassWriteIndex = zone->_beginSlot;
            outTimestampWrites->endOfPassWriteIndex = zone->_endSlot;
            return true;
        }
        return false;
    }

    void VkmCommandBufferWebGPU::onResolveGpuZones(const uint32_t firstSlot, const uint32_t count)
    {
        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);
        WGPUQuerySet querySet = driverWebGPU->getGpuTimestampQuerySet();
        if (querySet == nullptr || count == 0)
        {
            return;
        }

        // A query set is not CPU-readable: it has to be resolved into a QueryResolve buffer on
        // the GPU timeline and then copied into a separate MapRead buffer, because WebGPU forbids
        // combining those two usages. VkmDriverWebGPU::resolveGpuTimestamps maps the second one
        // once this submission has completed.
        const uint64_t byteOffset = static_cast<uint64_t>(firstSlot) * sizeof(uint64_t);
        const uint64_t byteSize = static_cast<uint64_t>(count) * sizeof(uint64_t);
        wgpuCommandEncoderResolveQuerySet(_encoder, querySet, firstSlot, count,
                                          driverWebGPU->getGpuTimestampResolveBuffer(), byteOffset);
        wgpuCommandEncoderCopyBufferToBuffer(_encoder, driverWebGPU->getGpuTimestampResolveBuffer(), byteOffset,
                                             driverWebGPU->getGpuTimestampReadbackBuffer(), byteOffset, byteSize);
    }

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
    void VkmCommandBufferWebGPU::onWriteCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t offset)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        WGPUBuffer wgpuMarkerBuffer = static_cast<VkmStagingBufferWebGPU*>(renderResourcePool->getResource<VkmStagingBuffer>(markerBuffer))->getBuffer();
        WGPUBuffer wgpuOneBuffer = static_cast<VkmStagingBufferWebGPU*>(renderResourcePool->getResource<VkmStagingBuffer>(oneBuffer))->getBuffer();

        wgpuCommandEncoderCopyBufferToBuffer(_encoder, wgpuOneBuffer, 0, wgpuMarkerBuffer, offset, sizeof(uint32_t));
    }

    void VkmCommandBufferWebGPU::onEndCommandBuffer()
    {
        // No-op: onWriteCompletionMarker() already records its copyBufferToBuffer immediately
        // on _encoder, no batching needed.
    }
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS
} // namespace vkm
