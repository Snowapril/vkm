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
    
    void VkmCommandBufferBase::bindPipeline(VkmPipelineStateBase* pipelineState)
    {
        _boundPipelineState = pipelineState;
        onBindPipeline(pipelineState);
    }

    void VkmCommandBufferBase::unbindPipeline()
    {
        onUnbindPipeline();
        _boundPipelineState = nullptr;
    }

    void VkmCommandBufferBase::copyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size)
    {
        if (!_isRecording || _isInRenderPass)
        {
            VKM_DEBUG_ERROR("copyBuffer must be called while recording and outside a render pass");
            return;
        }
        onCopyBuffer(srcBuffer, dstBuffer, srcOffset, dstOffset, size);
    }

    void VkmCommandBufferBase::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        if (!_isRecording || !_isInRenderPass || _boundPipelineState == nullptr)
        {
            VKM_DEBUG_ERROR("draw requires an active render pass with a bound pipeline");
            return;
        }
        onDraw(vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void VkmCommandBufferBase::setPushConstants(const void* data, uint32_t size, uint32_t offset)
    {
        if (!_isRecording || !_isInRenderPass || _boundPipelineState == nullptr)
        {
            VKM_DEBUG_ERROR("setPushConstants requires an active render pass with a bound pipeline");
            return;
        }
        onSetPushConstants(data, size, offset);
    }
}