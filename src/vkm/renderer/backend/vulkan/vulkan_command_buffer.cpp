// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_command_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_staging_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_pipeline_state.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_bindless_resource_manager.h>
#include <vkm/renderer/backend/vulkan/vulkan_gpu_timer.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/driver.h>

namespace vkm
{
    static VkAttachmentLoadOp toVkAttachmentLoadOp(VkmLoadAction loadAction)
    {
        switch (loadAction)
        {
            case VkmLoadAction::Load:     return VK_ATTACHMENT_LOAD_OP_LOAD;
            case VkmLoadAction::Clear:    return VK_ATTACHMENT_LOAD_OP_CLEAR;
            case VkmLoadAction::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        }
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }

    static VkAttachmentStoreOp toVkAttachmentStoreOp(VkmStoreAction storeAction)
    {
        switch (storeAction)
        {
            case VkmStoreAction::Store:   return VK_ATTACHMENT_STORE_OP_STORE;
            case VkmStoreAction::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
        return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }

    // Dynamic rendering (vkCmdBeginRendering/vkCmdEndRendering) has no subpass dependencies to
    // transition image layouts implicitly, unlike a legacy VkRenderPass -- so every attachment
    // needs an explicit barrier between whatever it was doing before and how this render pass
    // (or present) is about to use it.
    static void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
    {
        const VkImageMemoryBarrier2 barrier{
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask       = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask       = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
            .oldLayout           = oldLayout,
            .newLayout           = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = image,
            .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        const VkDependencyInfo dependencyInfo{
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers    = &barrier,
        };
        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
    }

    // copyBuffer() is a generic buffer-to-buffer copy usable for either a Buffer or a
    // StagingBuffer resource on either side (e.g. VkmDriverBase::uploadToBuffer() copies
    // staging -> device-local) -- these are unrelated classes (VkmStagingBuffer doesn't
    // derive from VkmBuffer), so resolve via the handle's own recorded type rather than
    // assuming one.
    static VkBuffer resolveVkBufferAndOffset(VkmRenderResourcePool* renderResourcePool, VkmResourceHandle handle, uint64_t* outOffset)
    {
        if (handle.type == VkmResourceType::StagingBuffer)
        {
            VkmStagingBufferVulkan* stagingBufferVulkan = static_cast<VkmStagingBufferVulkan*>(renderResourcePool->getResource<VkmStagingBuffer>(handle));
            *outOffset = 0; // staging buffers are always dedicated allocations, never pooled
            return stagingBufferVulkan->getBuffer();
        }
        VkmBufferVulkan* bufferVulkan = static_cast<VkmBufferVulkan*>(renderResourcePool->getResource<VkmBuffer>(handle));
        *outOffset = bufferVulkan->getBufferOffset();
        return bufferVulkan->getBuffer();
    }

    VkmCommandBufferVulkan::VkmCommandBufferVulkan(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool)
        : VkmCommandBufferBase(driver, commandQueue, commandBufferPool)
    {
    }

    VkmCommandBufferVulkan::~VkmCommandBufferVulkan()
    {
    }

    void VkmCommandBufferVulkan::setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle)
    {
        _vkCommandBuffer = static_cast<VkCommandBuffer>(handle);
    }

    void VkmCommandBufferVulkan::onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();

        std::vector<VkRenderingAttachmentInfo> colorAttachments;
        colorAttachments.reserve(frameBufferDesc._renderPass._colorAttachmentCount);
        for (uint32_t i = 0; i < frameBufferDesc._renderPass._colorAttachmentCount; ++i)
        {
            VkmTextureVulkan* colorTextureVulkan = static_cast<VkmTextureVulkan*>(renderResourcePool->getResource<VkmTexture>(frameBufferDesc._colorAttachments[i]));
            const VkmColorAttachmentDescriptor& colorAttachmentDesc = frameBufferDesc._renderPass._colorAttachments[i];

            const VkImageLayout previousLayout = colorTextureVulkan->getCurrentLayout();
            if (previousLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            {
                transitionImageLayout(_vkCommandBuffer, colorTextureVulkan->getImage(), previousLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                colorTextureVulkan->setCurrentLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            }

            colorAttachments.push_back(VkRenderingAttachmentInfo{
                .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView   = colorTextureVulkan->getImageView(),
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp      = toVkAttachmentLoadOp(colorAttachmentDesc._loadAction),
                .storeOp     = toVkAttachmentStoreOp(colorAttachmentDesc._storeAction),
                .clearValue  = { .color = { .float32 = {
                    colorAttachmentDesc._clearColors[0],
                    colorAttachmentDesc._clearColors[1],
                    colorAttachmentDesc._clearColors[2],
                    colorAttachmentDesc._clearColors[3] } } },
            });
        }

        const VkRenderingInfo renderingInfo{
            .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea           = { {0, 0}, {frameBufferDesc._width, frameBufferDesc._height} },
            .layerCount           = 1,
            .colorAttachmentCount = (uint32_t)colorAttachments.size(),
            .pColorAttachments    = colorAttachments.data(),
        };
        vkCmdBeginRendering(_vkCommandBuffer, &renderingInfo);

        // Every pipeline created by VkmPipelineStateVulkan marks viewport/scissor as
        // dynamic state (extents aren't known at pipeline-creation time), so they must be
        // set here every render pass or drawing has undefined viewport/scissor state.
        const VkViewport viewport{
            .x = 0.0f, .y = 0.0f,
            .width = static_cast<float>(frameBufferDesc._width), .height = static_cast<float>(frameBufferDesc._height),
            .minDepth = 0.0f, .maxDepth = 1.0f,
        };
        const VkRect2D scissor{ {0, 0}, {frameBufferDesc._width, frameBufferDesc._height} };
        vkCmdSetViewport(_vkCommandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(_vkCommandBuffer, 0, 1, &scissor);
    }

    void VkmCommandBufferVulkan::onEndRenderPass()
    {
        vkCmdEndRendering(_vkCommandBuffer);

        // Transition presentable attachments back to PRESENT_SRC_KHR so the swapchain can
        // present them; non-presentable render targets (no AllowPresent flag) are left in
        // COLOR_ATTACHMENT_OPTIMAL for whatever consumes them next.
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        for (uint32_t i = 0; i < _currentFrameBufferDesc._renderPass._colorAttachmentCount; ++i)
        {
            VkmTextureVulkan* colorTextureVulkan = static_cast<VkmTextureVulkan*>(renderResourcePool->getResource<VkmTexture>(_currentFrameBufferDesc._colorAttachments[i]));
            if ((colorTextureVulkan->getTextureInfo()._flags & VkmResourceCreateInfo::AllowPresent) != 0)
            {
                transitionImageLayout(_vkCommandBuffer, colorTextureVulkan->getImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                colorTextureVulkan->setCurrentLayout(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            }
        }
    }

    void VkmCommandBufferVulkan::onBindPipeline(VkmPipelineStateBase* pipelineState)
    {
        VkmPipelineStateVulkan* pipelineStateVulkan = static_cast<VkmPipelineStateVulkan*>(pipelineState);
        const VkPipelineBindPoint bindPoint = pipelineState->isCompute() ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
        vkCmdBindPipeline(_vkCommandBuffer, bindPoint, pipelineStateVulkan->getHandle());

        _boundPipelineLayout = pipelineStateVulkan->getPipelineLayout();

        // Set 0 is the same engine-global bindless set for every pipeline in this
        // convention (see VkmPipelineStateVulkan::createInner), so bind it here rather than
        // asking every draw call site to do so explicitly.
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        VkDescriptorSet bindlessSet = driverVulkan->getBindlessResourceManager()->getDescriptorSet();
        vkCmdBindDescriptorSets(_vkCommandBuffer, bindPoint, _boundPipelineLayout, 0, 1, &bindlessSet, 0, nullptr);
    }

    void VkmCommandBufferVulkan::onUnbindPipeline()
    {
        // Vulkan has no explicit unbind concept -- nothing to do here.
        _boundPipelineLayout = VK_NULL_HANDLE;
    }

    void VkmCommandBufferVulkan::onCopyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        uint64_t srcBaseOffset = 0, dstBaseOffset = 0;
        VkBuffer srcVkBuffer = resolveVkBufferAndOffset(renderResourcePool, srcBuffer, &srcBaseOffset);
        VkBuffer dstVkBuffer = resolveVkBufferAndOffset(renderResourcePool, dstBuffer, &dstBaseOffset);

        const VkBufferCopy copyRegion{
            .srcOffset = srcOffset + srcBaseOffset,
            .dstOffset = dstOffset + dstBaseOffset,
            .size      = size,
        };
        vkCmdCopyBuffer(_vkCommandBuffer, srcVkBuffer, dstVkBuffer, 1, &copyRegion);
    }

    void VkmCommandBufferVulkan::onDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        vkCmdDraw(_vkCommandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void VkmCommandBufferVulkan::onSetPushConstants(const void* data, uint32_t size, uint32_t offset)
    {
        vkCmdPushConstants(_vkCommandBuffer, _boundPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, offset, size, data);
    }

    void VkmCommandBufferVulkan::writeGpuTimestampBegin()
    {
        static_cast<VkmDriverVulkan*>(_driver)->getGpuTimer()->writeBeginTimestamp(_vkCommandBuffer);
    }

    void VkmCommandBufferVulkan::writeGpuTimestampEnd()
    {
        static_cast<VkmDriverVulkan*>(_driver)->getGpuTimer()->writeEndTimestamp(_vkCommandBuffer);
    }

    void VkmCommandBufferVulkan::onWriteCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t offset)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        VkBuffer vkMarkerBuffer = static_cast<VkmStagingBufferVulkan*>(renderResourcePool->getResource<VkmStagingBuffer>(markerBuffer))->getBuffer();
        VkBuffer vkOneBuffer = static_cast<VkmStagingBufferVulkan*>(renderResourcePool->getResource<VkmStagingBuffer>(oneBuffer))->getBuffer();

        // Legal outside a render pass -- dynamic rendering has already ended by the time a
        // subgraph's commit() returns, which is the only place this is called from.
        const VkBufferCopy region{
            .srcOffset = 0,
            .dstOffset = offset,
            .size      = sizeof(uint32_t),
        };
        vkCmdCopyBuffer(_vkCommandBuffer, vkOneBuffer, vkMarkerBuffer, 1, &region);
    }

    void VkmCommandBufferVulkan::onSetDebugName(const char* name)
    {
#ifdef VKM_DEBUG_NAME_ENABLED
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        const VkDebugUtilsObjectNameInfoEXT nameInfo{
            .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType   = VK_OBJECT_TYPE_COMMAND_BUFFER,
            .objectHandle = reinterpret_cast<uint64_t>(_vkCommandBuffer),
            .pObjectName  = name,
        };
        vkSetDebugUtilsObjectNameEXT(driverVulkan->getDevice(), &nameInfo);
#else
        (void)name;
#endif
    }

    void VkmCommandBufferVulkan::onEndCommandBuffer()
    {
        // No-op: onWriteCompletionMarker() already records its vkCmdCopyBuffer immediately,
        // no batching needed outside a render pass.
    }
} // namespace vkm
