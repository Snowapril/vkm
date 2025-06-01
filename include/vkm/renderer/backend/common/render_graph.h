// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>

namespace vkm
{
    class VkmCommandBufferBase;
    class VkmDriverBase;
    
    enum class VkmRenderSubGraphType : uint8_t
    {
        Graphics,
        Compute,
        Transfer
    };

    class VkmRenderSubGraph
    {
    public:
        VkmRenderSubGraph(VkmRenderSubGraphType subGraphType, uint32_t subGraphId)
            : _subGraphType(subGraphType), _subGraphId(subGraphId) {}
        virtual ~VkmRenderSubGraph() {};

        // Method to get the type of the subgraph
        inline VkmRenderSubGraphType getSubGraphType() const { return _subGraphType; }
        // Method to get the unique identifier for the subgraph
        inline uint32_t getSubGraphId() const { return _subGraphId; }
        // List of subgraphs that this subgraph depends on
        void addDependentSubGraphId(uint32_t subGraphId)
        {
            _dependentSubGraphIds.push_back(subGraphId);
        }
        void addDependentSubGraphIds(const std::vector<uint32_t>& subGraphIds)
        {
            _dependentSubGraphIds.insert(_dependentSubGraphIds.end(), subGraphIds.begin(), subGraphIds.end());
        }

    private:
        VkmRenderSubGraphType _subGraphType; // Type of the subgraph (Graphics, Compute, Transfer)
        uint32_t _subGraphId; // Unique identifier for the subgraph

        // List of command buffers associated with this subgraph
        std::vector<uint32_t> _dependentSubGraphIds; // IDs of subgraphs that this subgraph depends on
    };

    class VkmRenderGraphicsSubGraph : public VkmRenderSubGraph
    {
    public:
        // Frame buffer key for the graphics subgraph
        VkmRenderGraphicsSubGraph(uint32_t subGraphId) : VkmRenderSubGraph(VkmRenderSubGraphType::Graphics, subGraphId) {}
        ~VkmRenderGraphicsSubGraph() override = default;
    };
    class VkmRenderComputeSubGraph : public VkmRenderSubGraph
    {
    public:
        // Frame buffer key for the compute subgraph
        VkmRenderComputeSubGraph(uint32_t subGraphId) : VkmRenderSubGraph(VkmRenderSubGraphType::Compute, subGraphId) {}
        ~VkmRenderComputeSubGraph() override = default;
    };
    class VkmRenderTransferSubGraph : public VkmRenderSubGraph
    {
    public:
        // Frame buffer key for the transfer subgraph
        VkmRenderTransferSubGraph(uint32_t subGraphId) : VkmRenderSubGraph(VkmRenderSubGraphType::Transfer, subGraphId) {}
        ~VkmRenderTransferSubGraph() override = default;
    };

    class VkmRenderGraph
    {
    public:
        VkmRenderGraph(VkmDriverBase* driver, uint32_t frameIndex)
            : _driver(driver), _frameIndex(frameIndex) {}
        ~VkmRenderGraph() = default;

        // Method to add a subgraph to the render graph
        VkmRenderGraphicsSubGraph* beginGraphicsSubGraph();
        // Method to add a compute subgraph to the render graph
        VkmRenderComputeSubGraph* beginComputeSubGraph();
        // Method to add a transfer subgraph to the render graph
        VkmRenderTransferSubGraph* beginTransferSubGraph();

        void compile();
        void execute(VkmCommandBufferBase* commandBuffer);

        void setBackBuffer(VkmResourceHandle backBuffer)
        {
            // Set the back buffer for the render graph
            _backBuffer = backBuffer;
        }
        VkmResourceHandle getBackBuffer() const
        {
            // Get the back buffer for the render graph
            return _backBuffer;
        }

    private:
        VkmDriverBase* _driver; // Pointer to the driver managing this render graph
        uint32_t _frameIndex; // Frame index for this render graph
        std::vector<std::unique_ptr<VkmRenderSubGraph>> _subGraphs;
        uint32_t _currentSubGraphId = 0; // Current subgraph ID for tracking
        VkmResourceHandle _backBuffer; // Handle to the back buffer for rendering
    };
}