
// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/command_buffer.h>

namespace vkm
{
    VkmRenderGraphicsSubGraph* VkmRenderGraph::beginGraphicsSubGraph()
    {
        auto subGraph = std::make_unique<VkmRenderGraphicsSubGraph>(_currentSubGraphId++);
        VkmRenderGraphicsSubGraph* subGraphPtr = subGraph.get();
        _subGraphs.push_back(std::move(subGraph));
        return subGraphPtr;
    }

    VkmRenderComputeSubGraph* VkmRenderGraph::beginComputeSubGraph()
    {
        auto subGraph = std::make_unique<VkmRenderComputeSubGraph>(_currentSubGraphId++);
        VkmRenderComputeSubGraph* subGraphPtr = subGraph.get();
        _subGraphs.push_back(std::move(subGraph));
        return subGraphPtr;
    }

    VkmRenderTransferSubGraph* VkmRenderGraph::beginTransferSubGraph()
    {
        auto subGraph = std::make_unique<VkmRenderTransferSubGraph>(_currentSubGraphId++);
        VkmRenderTransferSubGraph* subGraphPtr = subGraph.get();
        _subGraphs.push_back(std::move(subGraph));
        return subGraphPtr;
    }

    void VkmRenderGraph::compile()
    {
        // Compile the render graph by processing all subgraphs
        // for (const auto& subGraph : _subGraphs)
        // {
        // }
        // Reset the current subgraph ID for the next frame
        _currentSubGraphId = 0;
    }

    void VkmRenderGraph::execute(VkmCommandBufferBase* commandBuffer)
    {
        
    }
}