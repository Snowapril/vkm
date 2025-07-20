// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/command_buffer.h>

namespace vkm
{
    VkmCommandBufferBase::VkmCommandBufferBase(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool)
        : _driver(driver), _commandQueue(commandQueue), _commandBufferPool(commandBufferPool), _isRecording(false), _isInRenderPass(false)
    {
    }

    VkmCommandBufferBase::~VkmCommandBufferBase()
    {

    }

    void VkmCommandBufferBase::beginCommandBuffer()
    {
        _isRecording = true;
        _isInRenderPass = false;
        // Reset the current frame buffer descriptor
        _currentFrameBufferDesc = {};

        VkmGpuEventTimelineBase* gpuEventTimeline = _commandQueue->getGpuEventTimeline();
        _gpuEventTimelineObject = gpuEventTimeline->allocateGpuEventTimelineObject();
    }
    
    void VkmCommandBufferBase::endCommandBuffer()
    {
        _isRecording = false;
        _isInRenderPass = false;
    }
    
    void VkmCommandBufferBase::beginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc)
    {
        if (!_isRecording)
        {
            VKM_DEBUG_ERROR("Cannot begin render pass when command buffer is not recording");
            return;
        }
        _isInRenderPass = true;
        // Here we would typically set up the render pass state and bind the frame buffer
        // For now, we just log the beginning of the render pass
        VKM_DEBUG_LOG("Beginning render pass with frame buffer descriptor");

        _currentFrameBufferDesc = frameBufferDesc;

        onBeginRenderPass(frameBufferDesc);
    }

    void VkmCommandBufferBase::endRenderPass()
    {
        if (!_isInRenderPass)
        {
            VKM_DEBUG_ERROR("Cannot end render pass when not in a render pass");
            return;
        }
        _isInRenderPass = false;
        // Here we would typically finalize the render pass state
        // For now, we just log the end of the render pass
        VKM_DEBUG_LOG("Ending render pass");

        onEndRenderPass();
    }
    
    void VkmCommandBufferBase::bindPipeline()
    {
    }

    void VkmCommandBufferBase::unbindPipeline()
    {
    }
}