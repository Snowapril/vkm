
// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>

namespace vkm
{
    void VkmRenderGraphicsSubGraph::commit(VkmCommandBufferBase* commandBuffer)
    {
        commandBuffer->beginRenderPass(_frameBufferDesc);
        // Draw/Dispatch commands would go here
        // For example, binding pipelines, setting viewport, scissor, etc.
        // commandBuffer->bindPipeline(pipeline);
        // commandBuffer->drawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
        // commandBuffer->dispatch(groupCountX, groupCountY, groupCountZ);
        // ...  
        commandBuffer->endRenderPass();
    }

    void VkmRenderComputeSubGraph::commit(VkmCommandBufferBase* commandBuffer)
    {
    }

    void VkmRenderTransferSubGraph::commit(VkmCommandBufferBase* commandBuffer)
    {
    }

    VkmRenderGraphicsSubGraph* VkmRenderGraph::beginGraphicsSubGraph(const VkmFrameBufferDescriptor& desc)
    {
        // Create a new graphics subgraph with the provided framebuffer descriptor
        return beginSubGraph<VkmRenderGraphicsSubGraph>(desc);
    }

    VkmRenderComputeSubGraph* VkmRenderGraph::beginComputeSubGraph()
    {
        return beginSubGraph<VkmRenderComputeSubGraph>();
    }

    VkmRenderTransferSubGraph* VkmRenderGraph::beginTransferSubGraph()
    {
        return beginSubGraph<VkmRenderTransferSubGraph>();
    }

    void VkmRenderGraph::compile(const VkmRenderGraphCompileOptions& options)
    {
        // Compile the render graph by processing all subgraphs
        // for (const auto& subGraph : _subGraphs)
        // {
        // }
        // Reset the current subgraph ID for the next frame

        (void)options; // Suppress unused variable warning for now
    }

    void VkmRenderGraph::execute(const VkmRenderGraphCommitOptions& options)
    {
        VkmCommandQueueBase* commandQueue = _driver->getCommandQueue(VkmCommandQueueType::Graphics, 0);
        VkmCommandBufferPoolBase* commandBufferPool = commandQueue->getCommandBufferPool();

        VkmCommandBufferBase* commandBuffer = commandBufferPool->allocate();
        commandBuffer->beginCommandBuffer();

        for (auto& subGraph : _subGraphs)
        {
            // Execute each subgraph's commands
            if (subGraph->getSubGraphType() == VkmRenderSubGraphType::Graphics)
            {
                
            }
            subGraph->commit(commandBuffer);
        }

        commandBuffer->endCommandBuffer();
        
        CommandSubmitInfo submitInfo;
        submitInfo.commandBuffers[0] = commandBuffer;
        submitInfo.commandBufferCount = 1;
        
        _lastSubmitInfo = commandQueue->submit(submitInfo);
        (void)options; // Suppress unused variable warning at now
    }

    void VkmRenderGraph::reset()
    {
        // Reset the render graph state for the next frame
        _subGraphs.clear();
        _currentSubGraphId = 0;
    }

    void VkmRenderGraph::ensureCompleted()
    {
        // Ensure that the last submitted command buffer has completed execution
        if (_lastSubmitInfo._gpuEventTimeline)
        {
            _lastSubmitInfo._gpuEventTimeline->waitIdle(MAX_GPU_TIMEOUT_PER_FRAME); // Wait for 1000 ms
        }
    }
}
