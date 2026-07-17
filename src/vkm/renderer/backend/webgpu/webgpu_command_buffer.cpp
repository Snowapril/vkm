// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_command_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_texture.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>
#include <vkm/renderer/backend/webgpu/webgpu_pipeline_state.h>
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

        const WGPURenderPassDescriptor renderPassDesc{
            .colorAttachmentCount = colorAttachments.size(),
            .colorAttachments     = colorAttachments.data(),
        };
        _renderPassEncoder = wgpuCommandEncoderBeginRenderPass(_encoder, &renderPassDesc);

        // wgpuCommandEncoderBeginRenderPass only needs the views for the duration of this call.
        for (WGPUTextureView view : colorViews)
        {
            wgpuTextureViewRelease(view);
        }
    }

    void VkmCommandBufferWebGPU::onEndRenderPass()
    {
        wgpuRenderPassEncoderEnd(_renderPassEncoder);
        wgpuRenderPassEncoderRelease(_renderPassEncoder);
        _renderPassEncoder = nullptr;
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
                _computePassEncoder = wgpuCommandEncoderBeginComputePass(_encoder, nullptr);
            }
            wgpuComputePassEncoderSetPipeline(_computePassEncoder, pipelineStateWebGPU->getComputePipeline());
            return;
        }

        VKM_ASSERT(_renderPassEncoder != nullptr, "onBindPipeline called for a graphics pipeline outside an active render pass");
        wgpuRenderPassEncoderSetPipeline(_renderPassEncoder, pipelineStateWebGPU->getRenderPipeline());

        // Every graphics pipeline shares the engine-global bindless bind group 0; bind it
        // with a zero dynamic offset as a safe default so draws that never push constants
        // are still valid (mirrors VkmCommandBufferVulkan::onBindPipeline's set-0 bind).
        VkmBindlessResourceManagerWebGPU* bindlessManager =
            static_cast<VkmDriverWebGPU*>(_driver)->getBindlessResourceManager();
        const uint32_t zeroOffset = 0;
        wgpuRenderPassEncoderSetBindGroup(_renderPassEncoder, 0, bindlessManager->getBindGroup(), 1, &zeroOffset);
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

    void VkmCommandBufferWebGPU::onCopyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset)
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
        source.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferInfo destination{};
        destination.layout.offset = dstOffset;
        destination.layout.bytesPerRow = bytesPerRow;
        destination.layout.rowsPerImage = textureInfo._extent.y;
        destination.buffer = resolveWGPUBuffer(renderResourcePool, dstBuffer);

        const WGPUExtent3D copySize{textureInfo._extent.x, textureInfo._extent.y, 1};
        wgpuCommandEncoderCopyTextureToBuffer(_encoder, &source, &destination, &copySize);
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
        wgpuRenderPassEncoderSetBindGroup(_renderPassEncoder, 0, bindlessManager->getBindGroup(), 1, &dynamicOffset);
    }

    void VkmCommandBufferWebGPU::onSetDebugName(const char* name)
    {
        wgpuCommandEncoderSetLabel(_encoder, toWGPUStringView(name));
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
