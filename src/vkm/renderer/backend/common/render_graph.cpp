
// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>

namespace vkm
{
    void VkmRenderGraphicsSubGraph::commit(VkmCommandBufferBase* commandBuffer)
    {
        commandBuffer->beginRenderPass(_frameBufferDesc);
        if (_renderCallback)
        {
            _renderCallback(commandBuffer);
        }
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
        commandBuffer->writeGpuTimestampBegin();

        for (auto& subGraph : _subGraphs)
        {
            // Execute each subgraph's commands
            if (subGraph->getSubGraphType() == VkmRenderSubGraphType::Graphics)
            {

            }
            subGraph->commit(commandBuffer);
        }

        commandBuffer->writeGpuTimestampEnd();
        commandBuffer->endCommandBuffer();
        
        CommandSubmitInfo submitInfo;
        submitInfo.commandBuffers[0] = commandBuffer;
        submitInfo.commandBufferCount = 1;
        
        _lastSubmitInfo = commandQueue->submit(submitInfo);

        // TODO(snowapril) : execute() itself only ever submits to the Graphics queue (see
        // getCommandQueue(Graphics, 0) above) -- once it dispatches subgraphs to multiple queue
        // types, this loop will automatically record the correct per-instance usage regardless,
        // since recordUsage() is now keyed by timeline identity rather than queue type.
        VkmRenderResourcePool* resourcePool = _driver->getRenderResourcePool();
        for (auto& subGraph : _subGraphs)
        {
            for (VkmResourceHandle handle : subGraph->getReferencedResources())
            {
                VkmRenderResource* resource = resourcePool->getResource<VkmRenderResource>(handle);
                if (resource != nullptr)
                {
                    resource->recordUsage(_lastSubmitInfo);
                }
            }
        }

        // On WASM (no dedicated reclaimer thread) this is the only mechanism driving
        // deferred destruction; on Vulkan/Metal it's a harmless redundant sweep alongside
        // the background thread that already does the real work.
        _driver->getDeferredReclaimer()->pollOnce();

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
