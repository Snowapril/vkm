// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_command_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_texture.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>
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
} // namespace vkm
