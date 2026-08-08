// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <string>
#include <vector>

namespace vkm
{
    // Declared in render_graph.h. Forward-declared rather than included so this stays a leaf
    // header the analysis and its unit test can use without pulling in the graph itself.
    enum class VkmRenderSubGraphType : uint8_t;

    /*
    * @brief One resource dependency, as the command buffer is asked to record it.
    *
    * @details Names both ends: what the producing work did to the resource and what the consuming
    * work is about to do. A backend needs both -- Vulkan turns the pair into src/dst stage and
    * access masks plus a layout transition, Metal into a pair of MTLStages masks -- which is why
    * this carries a source access rather than only a destination like the entry points it replaces.
    */
    struct VkmResourceBarrier
    {
        VkmResourceHandle _handle = VKM_INVALID_RESOURCE_HANDLE;
        VkmResourceAccess _srcAccess = VkmResourceAccess::None;
        VkmResourceAccess _dstAccess = VkmResourceAccess::None;
        VkmPipelineScope _srcScope = VkmPipelineScope::All;
        VkmPipelineScope _dstScope = VkmPipelineScope::All;
        VkmSubresourceRange _range;
        /*
        * Write-after-read: the source only read, so the two sides need ordering but no cache
        * flush. Vulkan drops the source access mask, Metal drops to MTL4VisibilityOptionNone.
        */
        bool _executionOnly = false;
    };

    /*
    * @brief What one subgraph declares about one resource, as the analysis sees it. Mirrors
    * VkmResourceAccessDeclaration plus the scope its subgraph type implies.
    */
    struct VkmSubGraphAccess
    {
        VkmResourceHandle _handle = VKM_INVALID_RESOURCE_HANDLE;
        VkmResourceAccess _access = VkmResourceAccess::None;
        VkmSubresourceRange _range;
    };

    /*
    * @brief One subgraph's input to the analysis.
    *
    * @details Plain data, not a VkmRenderSubGraph pointer, so the analysis can be exercised
    * without a driver, a resource pool or a GPU -- which is what makes it worth unit testing at
    * all. VkmRenderGraph::compile() fills these in from its subgraphs.
    */
    struct VkmSubGraphAccessView
    {
        uint32_t _subGraphId = INVALID_VALUE32;
        VkmPipelineScope _scope = VkmPipelineScope::All;
        const VkmSubGraphAccess* _accesses = nullptr;
        uint32_t _accessCount = 0;
    };

    /*
    * @brief Where the analysis says each barrier goes, indexed by subgraph position.
    *
    * @details Two lists per subgraph rather than one. `_acquire[i]` is what has to be made visible
    * before subgraph i runs; `_release[i]` is what subgraph i publishes for later subgraphs. Both
    * are handed to the command buffer *before* subgraph i commits, because Metal's release half
    * must be recorded inside the producing encoder and that encoder is already closed by the time
    * commit() returns. Each backend then places them where its API needs them.
    */
    struct VkmRenderGraphBarrierPlan
    {
        std::vector<std::vector<VkmResourceBarrier>> _acquire;
        std::vector<std::vector<VkmResourceBarrier>> _release;
        // Producer subgraph *ids* per subgraph position, deduplicated -- what feeds
        // VkmRenderSubGraph::addDependentSubGraphIds.
        std::vector<std::vector<uint32_t>> _dependencies;
        // Populated only when validation is on; each entry names the subgraph and the problem.
        std::vector<std::string> _validationErrors;
    };

    /*
    * @brief How many subresources a resource has, so a range can be resolved and a whole-resource
    * range recognised. Buffers, samplers and acceleration structures report 1 x 1.
    *
    * VkmRenderGraph::compile() backs this with its resource pool; a test backs it with a map,
    * which is the point of it being an interface.
    */
    class VkmResourceSubresourceLookup
    {
    public:
        virtual ~VkmResourceSubresourceLookup() = default;
        // False when the handle names nothing live, which the caller reports as a validation error.
        virtual bool getSubresourceCounts(VkmResourceHandle handle, uint32_t* outMipLevels,
                                          uint32_t* outArrayLayers) const = 0;
    };

    /*
    * @brief Walks the subgraphs in order and works out which of them have to be ordered against
    * each other, and with what barrier.
    *
    * @details A hazard exists between a prior access and a new one when either writes, or when the
    * two need different image layouts. Read-after-read at one layout needs nothing, which is the
    * case worth getting right: a G-buffer sampled by four passes in a row should cost one barrier,
    * not four.
    *
    * **No entry state.** What a resource carried in from a previous frame, a host upload or a
    * swapchain acquire is backend knowledge -- Vulkan already tracks it per texture -- so a
    * resource's first access here simply gets `_srcAccess = None`, and the backend resolves the
    * real prior state itself. That is what keeps this a pure function of the declarations.
    *
    * `options.optimize` gates the read-after-read merge and the adjacent-subgraph collapse;
    * turning it off emits every barrier separately, which is the switch for bisecting a
    * synchronisation bug. `options.validate` gates `_validationErrors`.
    */
    VkmRenderGraphBarrierPlan vkmBuildRenderGraphBarrierPlan(const VkmSubGraphAccessView* subGraphs,
                                                             uint32_t subGraphCount,
                                                             const VkmResourceSubresourceLookup& lookup,
                                                             bool optimize, bool validate);

    // The scope a subgraph's work runs in. A graphics subgraph can only draw, a compute one can
    // only dispatch and a transfer one can only copy, so its type is an exact answer.
    VkmPipelineScope vkmPipelineScopeOfSubGraph(VkmRenderSubGraphType type);
}
