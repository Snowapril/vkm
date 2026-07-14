// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_command_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_texture.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>
#include <vkm/renderer/backend/webgpu/webgpu_pipeline_state.h>
#include <vkm/renderer/backend/webgpu/webgpu_staging_buffer.h>
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

    void VkmCommandBufferWebGPU::onCopyBuffer(VkmResourceHandle, VkmResourceHandle, uint64_t, uint64_t, uint64_t)
    {
        // Not implemented yet on WebGPU -- bindless resource upload is Vulkan-only for now.
        VKM_DEBUG_ERROR("VkmCommandBufferWebGPU::onCopyBuffer is not implemented");
    }

    void VkmCommandBufferWebGPU::onDraw(uint32_t, uint32_t, uint32_t, uint32_t)
    {
        // Not implemented yet on WebGPU -- bindless draw-call recording is Vulkan-only for now.
        VKM_DEBUG_ERROR("VkmCommandBufferWebGPU::onDraw is not implemented");
    }

    void VkmCommandBufferWebGPU::onSetPushConstants(const void*, uint32_t, uint32_t)
    {
        // Not implemented yet on WebGPU -- bindless push constants are Vulkan-only for now.
        VKM_DEBUG_ERROR("VkmCommandBufferWebGPU::onSetPushConstants is not implemented");
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
