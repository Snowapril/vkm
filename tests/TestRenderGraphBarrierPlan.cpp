#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/render_graph_barrier.h>
// Only for VkmRenderSubGraphType, which render_graph_barrier.h forward-declares so it can stay a
// leaf header; the analysis itself needs nothing from the graph.
#include <vkm/renderer/backend/common/render_graph.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

/*
* The render graph's dependency analysis, exercised without a driver, a resource pool or a GPU.
*
* vkmBuildRenderGraphBarrierPlan takes plain declarations and a subresource-count lookup, which is
* exactly what makes this possible -- and it is why the analysis was written that way. Every case
* below is a hazard the engine actually produces: a G-buffer written then sampled, a cull dispatch
* feeding an indirect draw, one probe atlas face updated while the rest keep their contents.
*
* These run on every backend and every CI job, including ones with no usable device.
*/

using vkm::VkmPipelineScope;
using vkm::VkmResourceAccess;
using vkm::VkmResourceHandle;
using vkm::VkmResourceType;
using vkm::VkmSubGraphAccess;
using vkm::VkmSubGraphAccessView;
using vkm::VkmSubresourceRange;

namespace
{
// Answers subresource counts from a map, standing in for the render resource pool.
class FakeLookup : public vkm::VkmResourceSubresourceLookup
{
public:
    void add(VkmResourceHandle handle, uint32_t mipLevels = 1, uint32_t arrayLayers = 1)
    {
        _counts[handle.id] = { mipLevels, arrayLayers };
    }

    bool getSubresourceCounts(VkmResourceHandle handle, uint32_t* outMipLevels,
                              uint32_t* outArrayLayers) const override
    {
        const auto it = _counts.find(handle.id);
        if (it == _counts.end())
        {
            return false;
        }
        *outMipLevels = it->second.first;
        *outArrayLayers = it->second.second;
        return true;
    }

private:
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> _counts;
};

VkmResourceHandle makeHandle(uint64_t id, VkmResourceType type)
{
    return VkmResourceHandle{ id, vkm::VkmResourcePoolType::Default, type, 0 };
}

// Builds the flat access array plus the views that point into it, the shape
// VkmRenderGraph::compile() hands the analysis.
struct GraphBuilder
{
    std::vector<std::vector<VkmSubGraphAccess>> _perSubGraph;
    std::vector<VkmPipelineScope> _scopes;

    uint32_t addSubGraph(VkmPipelineScope scope)
    {
        _perSubGraph.emplace_back();
        _scopes.push_back(scope);
        return static_cast<uint32_t>(_perSubGraph.size() - 1);
    }

    void declare(uint32_t subGraph, VkmResourceHandle handle, VkmResourceAccess access,
                 const VkmSubresourceRange& range = {})
    {
        _perSubGraph[subGraph].push_back(VkmSubGraphAccess{ handle, access, range });
    }

    vkm::VkmRenderGraphBarrierPlan build(const FakeLookup& lookup, bool optimize = true,
                                         bool validate = true) const
    {
        std::vector<VkmSubGraphAccessView> views;
        views.reserve(_perSubGraph.size());
        for (size_t i = 0; i < _perSubGraph.size(); ++i)
        {
            VkmSubGraphAccessView view{};
            view._subGraphId = static_cast<uint32_t>(i);
            view._scope = _scopes[i];
            view._accesses = _perSubGraph[i].data();
            view._accessCount = static_cast<uint32_t>(_perSubGraph[i].size());
            views.push_back(view);
        }
        return vkm::vkmBuildRenderGraphBarrierPlan(views.data(), static_cast<uint32_t>(views.size()),
                                                   lookup, optimize, validate);
    }
};
} // namespace

/*
* The archetypal case the whole change exists for: a compute pass writes the indirect argument
* buffer and the next pass fetches draws out of it. Before this analysis, the only thing ordering
* those two was a hand-placed barrierIndirectArgumentBuffer() call inside VkmScene::recordCull.
*/
TEST_CASE("VkmRenderGraphBarrierPlan - a write then a read produces one acquire on the reader") {
    const VkmResourceHandle argumentBuffer = makeHandle(1, VkmResourceType::Buffer);
    FakeLookup lookup;
    lookup.add(argumentBuffer);

    GraphBuilder graph;
    const uint32_t cull = graph.addSubGraph(VkmPipelineScope::Compute);
    const uint32_t draw = graph.addSubGraph(VkmPipelineScope::Graphics);
    graph.declare(cull, argumentBuffer, VkmResourceAccess::ShaderStorageWrite);
    graph.declare(draw, argumentBuffer, VkmResourceAccess::IndirectArgument);

    const vkm::VkmRenderGraphBarrierPlan plan = graph.build(lookup);

    CHECK(plan._acquire[cull].empty());
    REQUIRE(plan._acquire[draw].size() == 1);
    const vkm::VkmResourceBarrier& barrier = plan._acquire[draw][0];
    CHECK(barrier._handle == argumentBuffer);
    CHECK(barrier._srcAccess == VkmResourceAccess::ShaderStorageWrite);
    CHECK(barrier._dstAccess == VkmResourceAccess::IndirectArgument);
    CHECK(barrier._srcScope == VkmPipelineScope::Compute);
    CHECK(barrier._dstScope == VkmPipelineScope::Graphics);
    CHECK(barrier._executionOnly == false);

    // The dependency edge is what the render graph capture and its ImGui inspector display; before
    // this analysis nothing ever called addDependentSubGraphId, so that line was always empty.
    REQUIRE(plan._dependencies[draw].size() == 1);
    CHECK(plan._dependencies[draw][0] == cull);
    CHECK(plan._dependencies[cull].empty());
}

/*
* Adjacent producer and consumer collapse to an acquire only. Splitting buys nothing when there is
* no work between the two halves for the dependency to overlap with -- and on the Vulkan event
* path it would cost a VkEvent for that nothing.
*/
TEST_CASE("VkmRenderGraphBarrierPlan - an adjacent producer emits no release half") {
    const VkmResourceHandle buffer = makeHandle(1, VkmResourceType::Buffer);
    FakeLookup lookup;
    lookup.add(buffer);

    GraphBuilder graph;
    const uint32_t producer = graph.addSubGraph(VkmPipelineScope::Compute);
    const uint32_t consumer = graph.addSubGraph(VkmPipelineScope::Compute);
    graph.declare(producer, buffer, VkmResourceAccess::ShaderStorageWrite);
    graph.declare(consumer, buffer, VkmResourceAccess::ShaderStorageRead);

    const vkm::VkmRenderGraphBarrierPlan plan = graph.build(lookup);

    CHECK(plan._acquire[consumer].size() == 1);
    CHECK(plan._release[producer].empty());
}

// ... and a producer with work in between keeps both halves, which is what a split barrier is for.
TEST_CASE("VkmRenderGraphBarrierPlan - a non-adjacent producer keeps the release half") {
    const VkmResourceHandle buffer = makeHandle(1, VkmResourceType::Buffer);
    const VkmResourceHandle unrelated = makeHandle(2, VkmResourceType::Buffer);
    FakeLookup lookup;
    lookup.add(buffer);
    lookup.add(unrelated);

    GraphBuilder graph;
    const uint32_t producer = graph.addSubGraph(VkmPipelineScope::Compute);
    const uint32_t between = graph.addSubGraph(VkmPipelineScope::Compute);
    const uint32_t consumer = graph.addSubGraph(VkmPipelineScope::Graphics);
    graph.declare(producer, buffer, VkmResourceAccess::ShaderStorageWrite);
    graph.declare(between, unrelated, VkmResourceAccess::ShaderStorageWrite);
    graph.declare(consumer, buffer, VkmResourceAccess::ShaderStorageRead);

    const vkm::VkmRenderGraphBarrierPlan plan = graph.build(lookup);

    REQUIRE(plan._acquire[consumer].size() == 1);
    REQUIRE(plan._release[producer].size() == 1);
    CHECK(plan._release[producer][0]._handle == buffer);
    CHECK(plan._release[between].empty());
}

/*
* The redundancy this change exists to remove. A G-buffer target sampled by the lighting, probe and
* composite passes in a row costs one barrier, not one per reader: the write is published once to
* the graphics scope and every later graphics read sits behind that.
*/
TEST_CASE("VkmRenderGraphBarrierPlan - repeated reads of one write cost one barrier") {
    const VkmResourceHandle target = makeHandle(1, VkmResourceType::Texture);
    FakeLookup lookup;
    lookup.add(target);

    GraphBuilder graph;
    const uint32_t write = graph.addSubGraph(VkmPipelineScope::Graphics);
    const uint32_t readA = graph.addSubGraph(VkmPipelineScope::Graphics);
    const uint32_t readB = graph.addSubGraph(VkmPipelineScope::Graphics);
    const uint32_t readC = graph.addSubGraph(VkmPipelineScope::Graphics);
    graph.declare(write, target, VkmResourceAccess::ColorAttachmentWrite);
    graph.declare(readA, target, VkmResourceAccess::ShaderSampledRead);
    graph.declare(readB, target, VkmResourceAccess::ShaderSampledRead);
    graph.declare(readC, target, VkmResourceAccess::ShaderSampledRead);

    const vkm::VkmRenderGraphBarrierPlan plan = graph.build(lookup);

    // Only the first reader pays: it is the one that transitions the texture out of its attachment
    // layout, and the two after it want the very same layout.
    CHECK(plan._acquire[readA].size() == 1);
    CHECK(plan._acquire[readB].empty());
    CHECK(plan._acquire[readC].empty());
}

/*
* ... but a reader in a different scope does pay. Publishing an attachment write to a draw does not
* make it visible to a later dispatch: the stages differ, so the barrier has to be re-emitted.
*/
TEST_CASE("VkmRenderGraphBarrierPlan - a read in another scope re-publishes the write") {
    const VkmResourceHandle target = makeHandle(1, VkmResourceType::Texture);
    FakeLookup lookup;
    lookup.add(target);

    GraphBuilder graph;
    const uint32_t write = graph.addSubGraph(VkmPipelineScope::Graphics);
    const uint32_t drawRead = graph.addSubGraph(VkmPipelineScope::Graphics);
    const uint32_t dispatchRead = graph.addSubGraph(VkmPipelineScope::Compute);
    graph.declare(write, target, VkmResourceAccess::ColorAttachmentWrite);
    graph.declare(drawRead, target, VkmResourceAccess::ShaderSampledRead);
    graph.declare(dispatchRead, target, VkmResourceAccess::ShaderSampledRead);

    const vkm::VkmRenderGraphBarrierPlan plan = graph.build(lookup);

    CHECK(plan._acquire[drawRead].size() == 1);
    REQUIRE(plan._acquire[dispatchRead].size() == 1);
    CHECK(plan._acquire[dispatchRead][0]._dstScope == VkmPipelineScope::Compute);
}

/*
* Write-after-read has to wait for every reader since the last write, not just the latest one --
* they may all still be in flight. It needs ordering but no cache flush, since a read publishes
* nothing.
*/
TEST_CASE("VkmRenderGraphBarrierPlan - a write waits on every prior reader, execution-only") {
    const VkmResourceHandle buffer = makeHandle(1, VkmResourceType::Buffer);
    FakeLookup lookup;
    lookup.add(buffer);

    GraphBuilder graph;
    const uint32_t seed = graph.addSubGraph(VkmPipelineScope::Transfer);
    const uint32_t readA = graph.addSubGraph(VkmPipelineScope::Graphics);
    const uint32_t readB = graph.addSubGraph(VkmPipelineScope::Compute);
    const uint32_t overwrite = graph.addSubGraph(VkmPipelineScope::Transfer);
    graph.declare(seed, buffer, VkmResourceAccess::TransferWrite);
    graph.declare(readA, buffer, VkmResourceAccess::ShaderStorageRead);
    graph.declare(readB, buffer, VkmResourceAccess::ShaderStorageRead);
    graph.declare(overwrite, buffer, VkmResourceAccess::TransferWrite);

    const vkm::VkmRenderGraphBarrierPlan plan = graph.build(lookup);

    // Two write-after-read barriers plus the write-after-write against the seed.
    const std::vector<vkm::VkmResourceBarrier>& acquire = plan._acquire[overwrite];
    uint32_t executionOnlyCount = 0;
    for (const vkm::VkmResourceBarrier& barrier : acquire)
    {
        if (barrier._executionOnly)
        {
            ++executionOnlyCount;
        }
    }
    CHECK(executionOnlyCount == 2);
    CHECK(plan._dependencies[overwrite].size() == 3);
}

/*
* Per-subresource tracking, which is the reason a range exists at all. The probe updater refreshes
* one atlas cell and the capture uploads one cube face at a time; a barrier covering the whole
* image would transition layers whose contents have to survive.
*/
TEST_CASE("VkmRenderGraphBarrierPlan - disjoint array layers do not hazard against each other") {
    const VkmResourceHandle cubemap = makeHandle(1, VkmResourceType::Texture);
    FakeLookup lookup;
    lookup.add(cubemap, /*mipLevels=*/1, /*arrayLayers=*/6);

    GraphBuilder graph;
    const uint32_t writeFace2 = graph.addSubGraph(VkmPipelineScope::Transfer);
    const uint32_t readFace0 = graph.addSubGraph(VkmPipelineScope::Graphics);
    const uint32_t readFace2 = graph.addSubGraph(VkmPipelineScope::Graphics);
    graph.declare(writeFace2, cubemap, VkmResourceAccess::TransferWrite,
                  VkmSubresourceRange{ 0, 1, 2, 1 });
    graph.declare(readFace0, cubemap, VkmResourceAccess::ShaderSampledRead,
                  VkmSubresourceRange{ 0, 1, 0, 1 });
    graph.declare(readFace2, cubemap, VkmResourceAccess::ShaderSampledRead,
                  VkmSubresourceRange{ 0, 1, 2, 1 });

    const vkm::VkmRenderGraphBarrierPlan plan = graph.build(lookup);

    // Face 0 was never written here, so its read depends on nothing inside this graph.
    CHECK(plan._dependencies[readFace0].empty());
    // Face 2 was, so its read waits for that write and names only that layer.
    REQUIRE(plan._dependencies[readFace2].size() == 1);
    CHECK(plan._dependencies[readFace2][0] == writeFace2);
    REQUIRE(plan._acquire[readFace2].size() == 1);
    CHECK(plan._acquire[readFace2][0]._range._baseArrayLayer == 2);
    CHECK(plan._acquire[readFace2][0]._range._arrayLayerCount == 1);
}

// A whole-resource write covers every layer, so a later single-layer read still hazards against it.
TEST_CASE("VkmRenderGraphBarrierPlan - a whole-resource write covers a single-layer read") {
    const VkmResourceHandle atlas = makeHandle(1, VkmResourceType::Texture);
    FakeLookup lookup;
    lookup.add(atlas, /*mipLevels=*/1, /*arrayLayers=*/6);

    GraphBuilder graph;
    const uint32_t clear = graph.addSubGraph(VkmPipelineScope::Graphics);
    const uint32_t unrelated = graph.addSubGraph(VkmPipelineScope::Compute);
    const uint32_t read = graph.addSubGraph(VkmPipelineScope::Graphics);
    graph.declare(clear, atlas, VkmResourceAccess::ColorAttachmentWrite); // default range = all
    graph.declare(unrelated, makeHandle(2, VkmResourceType::Buffer), VkmResourceAccess::None);
    graph.declare(read, atlas, VkmResourceAccess::ShaderSampledRead, VkmSubresourceRange{ 0, 1, 4, 1 });

    const vkm::VkmRenderGraphBarrierPlan plan = graph.build(lookup);

    REQUIRE(plan._dependencies[read].size() == 1);
    CHECK(plan._dependencies[read][0] == clear);
    REQUIRE(plan._acquire[read].size() == 1);
    CHECK(plan._acquire[read][0]._range._baseArrayLayer == 4);
}

/*
* A texture's first touch in a frame still gets a barrier, because its layout came from outside the
* graph -- a host upload, a previous frame, or a freshly acquired swapchain image. The producer is
* unknown, which is the signal the backend uses to resolve the real prior layout from its own
* tracker rather than from the plan.
*/
TEST_CASE("VkmRenderGraphBarrierPlan - a texture's first touch acquires against no producer") {
    const VkmResourceHandle texture = makeHandle(1, VkmResourceType::Texture);
    FakeLookup lookup;
    lookup.add(texture);

    GraphBuilder graph;
    const uint32_t read = graph.addSubGraph(VkmPipelineScope::Graphics);
    graph.declare(read, texture, VkmResourceAccess::ShaderSampledRead);

    const vkm::VkmRenderGraphBarrierPlan plan = graph.build(lookup);

    REQUIRE(plan._acquire[read].size() == 1);
    CHECK(plan._acquire[read][0]._srcAccess == VkmResourceAccess::None);
    CHECK(plan._dependencies[read].empty());
}

// A buffer has no layout, so its first touch needs nothing: cross-frame visibility is the frame
// slot's timeline wait, not a barrier.
TEST_CASE("VkmRenderGraphBarrierPlan - a buffer's first read needs no barrier") {
    const VkmResourceHandle buffer = makeHandle(1, VkmResourceType::Buffer);
    FakeLookup lookup;
    lookup.add(buffer);

    GraphBuilder graph;
    const uint32_t read = graph.addSubGraph(VkmPipelineScope::Compute);
    graph.declare(read, buffer, VkmResourceAccess::ShaderStorageRead);

    CHECK(graph.build(lookup)._acquire[read].empty());
}

/*
* VkmResourceAccess::None is the documented "keep this alive, it participates in no hazard"
* declaration -- a sampler, or a handle held only so the deferred reclaimer waits for the frame.
*/
TEST_CASE("VkmRenderGraphBarrierPlan - a None declaration produces no barrier") {
    const VkmResourceHandle sampler = makeHandle(1, VkmResourceType::Sampler);
    FakeLookup lookup;
    lookup.add(sampler);

    GraphBuilder graph;
    const uint32_t a = graph.addSubGraph(VkmPipelineScope::Graphics);
    const uint32_t b = graph.addSubGraph(VkmPipelineScope::Graphics);
    graph.declare(a, sampler, VkmResourceAccess::None);
    graph.declare(b, sampler, VkmResourceAccess::None);

    const vkm::VkmRenderGraphBarrierPlan plan = graph.build(lookup);
    CHECK(plan._acquire[a].empty());
    CHECK(plan._acquire[b].empty());
    CHECK(plan._validationErrors.empty());
}

// A handle naming nothing live is reported rather than silently skipped -- nothing checked this
// before, and a stale handle used to no-op its way through recordUsage.
TEST_CASE("VkmRenderGraphBarrierPlan - a dead handle is a validation error") {
    FakeLookup lookup; // deliberately empty

    GraphBuilder graph;
    const uint32_t subGraph = graph.addSubGraph(VkmPipelineScope::Compute);
    graph.declare(subGraph, makeHandle(7, VkmResourceType::Buffer), VkmResourceAccess::ShaderStorageRead);

    const vkm::VkmRenderGraphBarrierPlan plan = graph.build(lookup, /*optimize=*/true, /*validate=*/true);
    CHECK(plan._validationErrors.size() == 1);
    CHECK(plan._acquire[subGraph].empty());
}

/*
* optimize=false is the bisection switch for "is this a barrier bug or a shader bug?" -- it drops
* the read-after-read merge and the adjacent collapse, so every declaration emits its barrier.
*/
TEST_CASE("VkmRenderGraphBarrierPlan - optimize=false emits every barrier separately") {
    const VkmResourceHandle target = makeHandle(1, VkmResourceType::Texture);
    FakeLookup lookup;
    lookup.add(target);

    GraphBuilder graph;
    const uint32_t write = graph.addSubGraph(VkmPipelineScope::Graphics);
    const uint32_t readA = graph.addSubGraph(VkmPipelineScope::Graphics);
    const uint32_t readB = graph.addSubGraph(VkmPipelineScope::Graphics);
    graph.declare(write, target, VkmResourceAccess::ColorAttachmentWrite);
    graph.declare(readA, target, VkmResourceAccess::ShaderSampledRead);
    graph.declare(readB, target, VkmResourceAccess::ShaderSampledRead);

    const vkm::VkmRenderGraphBarrierPlan optimized = graph.build(lookup, /*optimize=*/true);
    CHECK(optimized._acquire[readB].empty());

    const vkm::VkmRenderGraphBarrierPlan conservative = graph.build(lookup, /*optimize=*/false);
    CHECK(conservative._acquire[readB].size() == 1);
    // The adjacent collapse is off too, so the producer keeps its release half.
    CHECK(conservative._release[write].empty() == false);
}

// The scope a subgraph runs in is exactly its type: a graphics subgraph can only draw, a compute
// one can only dispatch, a transfer one can only copy.
TEST_CASE("VkmRenderGraphBarrierPlan - the pipeline scope follows the subgraph type") {
    CHECK(vkm::vkmPipelineScopeOfSubGraph(vkm::VkmRenderSubGraphType::Graphics) == VkmPipelineScope::Graphics);
    CHECK(vkm::vkmPipelineScopeOfSubGraph(vkm::VkmRenderSubGraphType::Compute) == VkmPipelineScope::Compute);
    CHECK(vkm::vkmPipelineScopeOfSubGraph(vkm::VkmRenderSubGraphType::Transfer) == VkmPipelineScope::Transfer);
}
