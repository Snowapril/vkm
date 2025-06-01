
// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/command_buffer.h>

namespace vkm
{
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

    void VkmRenderGraph::compile()
    {
        // Compile the render graph by processing all subgraphs
        // for (const auto& subGraph : _subGraphs)
        // {
        // }
        // Reset the current subgraph ID for the next frame
    }

    void VkmRenderGraph::execute(VkmCommandBufferBase* commandBuffer)
    {
        
    }

    void VkmRenderGraph::reset()
    {
        // Reset the render graph state for the next frame
        _subGraphs.clear();
        _currentSubGraphId = 0;
    }
}