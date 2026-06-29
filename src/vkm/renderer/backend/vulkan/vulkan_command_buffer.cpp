// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_command_buffer.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/renderer_common.h>

namespace vkm
{
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
        std::vector<VkRenderingAttachmentInfo> colorAttachments;
        colorAttachments.reserve(frameBufferDesc._renderPass._colorAttachmentCount);
        for (uint32_t i = 0; i < frameBufferDesc._renderPass._colorAttachmentCount; ++i)
        {
            colorAttachments.push_back(VkRenderingAttachmentInfo{
                .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView   = VK_NULL_HANDLE,
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
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
    }

    void VkmCommandBufferVulkan::onEndRenderPass()
    {
        vkCmdEndRendering(_vkCommandBuffer);
    }
} // namespace vkm
