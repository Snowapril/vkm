// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/render_pass.h>
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
        VkmRenderGraphicsSubGraph(uint32_t subGraphId, const VkmFrameBufferDescriptor& desc) 
            : VkmRenderSubGraph(VkmRenderSubGraphType::Graphics, subGraphId), _frameBufferDesc(desc) {}
        ~VkmRenderGraphicsSubGraph() override = default;

    private:
        VkmFrameBufferDescriptor _frameBufferDesc; // Frame buffer descriptor for the graphics subgraph
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
        VkmRenderGraphicsSubGraph* beginGraphicsSubGraph(const VkmFrameBufferDescriptor& desc);
        // Method to add a compute subgraph to the render graph
        VkmRenderComputeSubGraph* beginComputeSubGraph();
        // Method to add a transfer subgraph to the render graph
        VkmRenderTransferSubGraph* beginTransferSubGraph();

        void compile();
        void execute(VkmCommandBufferBase* commandBuffer);
        void reset();

    private:
        template <typename RenderSubGraphT, typename... Arg>
        RenderSubGraphT* beginSubGraph(Arg&&... arg)
        {
            // Create a new subgraph of the specified type
            auto subGraph = std::make_unique<RenderSubGraphT>(_currentSubGraphId++, std::forward<Arg>(arg)...);
            // Add the subgraph to the list of subgraphs
            _subGraphs.push_back(std::move(subGraph));
            // Return a pointer to the newly created subgraph
            return static_cast<RenderSubGraphT*>(_subGraphs.back().get());
        }

    private:
        VkmDriverBase* _driver; // Pointer to the driver managing this render graph
        uint32_t _frameIndex; // Frame index for this render graph
        std::vector<std::unique_ptr<VkmRenderSubGraph>> _subGraphs;
        uint32_t _currentSubGraphId = 0; // Current subgraph ID for tracking
    };
}