// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/driver_resource.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/render_graph_barrier.h>
#include <functional>
#include <string>

namespace vkm
{
    class VkmCommandBufferBase;
    class VkmDriverBase;
    class VkmRenderGraphCapture;

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
            : _subGraphType(subGraphType), _subGraphId(subGraphId),
              _name("SubGraph#" + std::to_string(subGraphId)) {}
        virtual ~VkmRenderSubGraph() {};

        virtual void commit(VkmCommandBufferBase* commandBuffer) = 0;

    public:
        inline VkmRenderSubGraphType getSubGraphType() const { return _subGraphType; }
        inline uint32_t getSubGraphId() const { return _subGraphId; }
        // Human-readable label used by debug tooling such as render graph capture.
        void setName(const char* name) { _name = name; }
        const std::string& getName() const { return _name; }
        const std::vector<uint32_t>& getDependentSubGraphIds() const { return _dependentSubGraphIds; }
        void addDependentSubGraphId(uint32_t subGraphId)
        {
            _dependentSubGraphIds.push_back(subGraphId);
        }
        void addDependentSubGraphIds(const std::vector<uint32_t>& subGraphIds)
        {
            _dependentSubGraphIds.insert(_dependentSubGraphIds.end(), subGraphIds.begin(), subGraphIds.end());
        }
        // Replaces rather than appends, so a graph compiled twice lists each edge once.
        void setDependentSubGraphIds(std::vector<uint32_t> subGraphIds)
        {
            _dependentSubGraphIds = std::move(subGraphIds);
        }

        /*
        * @brief Declares how this subgraph touches a resource.
        * @details Serves two purposes. VkmRenderGraph::execute() tags the handle with the submit's
        * timeline value once committed, which keeps the resource alive while the GPU still reads
        * it; VkmRenderGraph::compile() derives the dependencies between subgraphs from the
        * accesses and places the barriers for them. A graphics subgraph's attachments are derived
        * from its frame buffer descriptor and must not be declared here.
        * @param handle Resource this subgraph touches.
        * @param access How it touches it. VkmResourceAccess::None declares lifetime only and takes
        * part in no hazard.
        * @param range Subresources covered; ignored for anything but a texture.
        */
        void addReferencedResource(VkmResourceHandle handle, VkmResourceAccess access,
                                   const VkmSubresourceRange& range = {})
        {
            _referencedResources.push_back(VkmResourceAccessDeclaration{ handle, access, range });
        }
        void addReferencedResources(const std::vector<VkmResourceAccessDeclaration>& declarations)
        {
            _referencedResources.insert(_referencedResources.end(), declarations.begin(), declarations.end());
        }
        const std::vector<VkmResourceAccessDeclaration>& getReferencedResources() const { return _referencedResources; }

    private:
        VkmRenderSubGraphType _subGraphType;
        uint32_t _subGraphId;
        std::string _name;

        std::vector<uint32_t> _dependentSubGraphIds;
        std::vector<VkmResourceAccessDeclaration> _referencedResources;
    };

    class VkmRenderGraphicsSubGraph : public VkmRenderSubGraph
    {
    public:
        VkmRenderGraphicsSubGraph(uint32_t subGraphId, const VkmFrameBufferDescriptor& desc)
            : VkmRenderSubGraph(VkmRenderSubGraphType::Graphics, subGraphId), _frameBufferDesc(desc) {}
        ~VkmRenderGraphicsSubGraph() override = default;

        void commit(VkmCommandBufferBase* commandBuffer) override final;

        // Callback invoked between beginRenderPass/endRenderPass during commit(), letting
        // app/engine code such as the ImGui overlay record draw commands into this subgraph.
        using VkmRenderCallback = std::function<void(VkmCommandBufferBase*)>;
        void setRenderCallback(VkmRenderCallback callback) { _renderCallback = std::move(callback); }

        const VkmFrameBufferDescriptor& getFrameBufferDescriptor() const { return _frameBufferDesc; }

    private:
        VkmFrameBufferDescriptor _frameBufferDesc;
        VkmRenderCallback _renderCallback;
    };
    class VkmRenderComputeSubGraph : public VkmRenderSubGraph
    {
    public:
        VkmRenderComputeSubGraph(uint32_t subGraphId) : VkmRenderSubGraph(VkmRenderSubGraphType::Compute, subGraphId) {}
        ~VkmRenderComputeSubGraph() override = default;

        void commit(VkmCommandBufferBase* commandBuffer) override final;

        /*
        * @brief Callback invoked during commit(), with no render pass open, so it may bind a
        * compute pipeline and dispatch.
        * @details The backing compute pass is opened by that bindPipeline() and closed by
        * unbindPipeline(), so a callback that dispatches must do both. Subgraphs commit in
        * insertion order into one command buffer, so a compute subgraph added before a graphics
        * subgraph publishes its writes ahead of the draws that read them.
        */
        using VkmComputeCallback = std::function<void(VkmCommandBufferBase*)>;
        void setComputeCallback(VkmComputeCallback callback) { _computeCallback = std::move(callback); }

    private:
        VkmComputeCallback _computeCallback;
    };
    class VkmRenderTransferSubGraph : public VkmRenderSubGraph
    {
    public:
        VkmRenderTransferSubGraph(uint32_t subGraphId) : VkmRenderSubGraph(VkmRenderSubGraphType::Transfer, subGraphId) {}
        ~VkmRenderTransferSubGraph() override = default;

        void commit(VkmCommandBufferBase* commandBuffer) override final;

        /*
        * @brief Callback invoked during commit(), with no render pass open, so it may record the
        * copies that a render pass forbids.
        * @details Subgraphs commit in insertion order into one command buffer, so a transfer
        * subgraph added before a graphics subgraph publishes its writes ahead of the draws that
        * read them.
        */
        using VkmTransferCallback = std::function<void(VkmCommandBufferBase*)>;
        void setTransferCallback(VkmTransferCallback callback) { _transferCallback = std::move(callback); }

    private:
        VkmTransferCallback _transferCallback;
    };
    
    struct VkmRenderGraphCompileOptions
    {
        bool optimize = true;
        bool validate = true;
    };

    struct VkmRenderGraphCommitOptions
    {
        bool waitForCompletion = true;
        // When non-null, the graph submit consumes this swapchain's present semaphores.
        VkmSwapChainBase* presentSwapChain = nullptr;
        // When non-null and armed, this frame's graph is recorded for debug inspection; see
        // render_graph_capture.h.
        VkmRenderGraphCapture* capture = nullptr;
    };

    class VkmRenderGraph
    {
    public:
        VkmRenderGraph(VkmDriverBase* driver, uint32_t frameIndex)
            : _driver(driver), _frameIndex(frameIndex) {}
        ~VkmRenderGraph() = default;

        VkmRenderGraphicsSubGraph* beginGraphicsSubGraph(const VkmFrameBufferDescriptor& desc, const char* name = nullptr);
        VkmRenderComputeSubGraph* beginComputeSubGraph(const char* name = nullptr);
        VkmRenderTransferSubGraph* beginTransferSubGraph(const char* name = nullptr);

        void compile(const VkmRenderGraphCompileOptions& options = {});
        void execute(const VkmRenderGraphCommitOptions& options = {});
        void reset();
        void ensureCompleted();

        uint32_t frameIndex() const { return _frameIndex; }

    private:
        template <typename RenderSubGraphT, typename... Arg>
        RenderSubGraphT* beginSubGraph(Arg&&... arg)
        {
            auto subGraph = std::make_unique<RenderSubGraphT>(_currentSubGraphId++, std::forward<Arg>(arg)...);
            _subGraphs.push_back(std::move(subGraph));
            return static_cast<RenderSubGraphT*>(_subGraphs.back().get());
        }

    private:
        VkmDriverBase* _driver;
        uint32_t _frameIndex;
        std::vector<std::unique_ptr<VkmRenderSubGraph>> _subGraphs;
        uint32_t _currentSubGraphId = 0;

        // Built by compile() from the subgraphs' declared accesses; cleared by reset().
        VkmRenderGraphBarrierPlan _barrierPlan;

        VkmGpuEventTimelineObject _lastSubmitInfo;
    };
}