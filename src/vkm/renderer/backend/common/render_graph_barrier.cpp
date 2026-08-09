// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/render_graph_barrier.h>
#include <vkm/renderer/backend/common/render_graph.h>

#include <algorithm>
#include <unordered_map>

namespace vkm
{
    namespace
    {
        /*
        * @brief The image layout an access needs, as a backend-neutral class.
        *
        * @details Only Vulkan has layouts, but the *analysis* still has to know when two accesses
        * disagree about one: a texture sampled and then written as a storage image needs a barrier
        * even though neither access is a write from the other's point of view. Metal and WebGPU
        * simply never see the answer.
        */
        enum class LayoutClass : uint8_t
        {
            // Buffers, samplers and acceleration structures -- nothing to transition, so two
            // accesses never disagree.
            None = 0,
            General,
            ShaderReadOnly,
            ColorAttachment,
            DepthStencilAttachment,
            DepthStencilReadOnly,
            TransferSrc,
            TransferDst,
            Present,
        };

        LayoutClass layoutClassOf(VkmResourceAccess access)
        {
            switch (access)
            {
                case VkmResourceAccess::ShaderSampledRead:           return LayoutClass::ShaderReadOnly;
                case VkmResourceAccess::ShaderStorageRead:
                case VkmResourceAccess::ShaderStorageWrite:
                case VkmResourceAccess::ShaderStorageReadWrite:      return LayoutClass::General;
                case VkmResourceAccess::ColorAttachmentWrite:        return LayoutClass::ColorAttachment;
                case VkmResourceAccess::DepthStencilAttachmentWrite: return LayoutClass::DepthStencilAttachment;
                case VkmResourceAccess::DepthStencilAttachmentRead:  return LayoutClass::DepthStencilReadOnly;
                case VkmResourceAccess::TransferRead:                return LayoutClass::TransferSrc;
                case VkmResourceAccess::TransferWrite:               return LayoutClass::TransferDst;
                case VkmResourceAccess::Present:                     return LayoutClass::Present;
                default:                                             return LayoutClass::None;
            }
        }

        // Only a texture has layouts to disagree about. A buffer read after a buffer write still
        // hazards, but never because of a layout.
        bool hasLayout(VkmResourceHandle handle)
        {
            return handle.type == VkmResourceType::Texture;
        }

        struct SubresourceState
        {
            // The last write, and who did it. INVALID_VALUE32 means nothing in this graph wrote it,
            // so the prior state comes from outside the frame and only the backend knows it.
            VkmResourceAccess _writeAccess = VkmResourceAccess::None;
            VkmPipelineScope _writeScope = VkmPipelineScope::All;
            uint32_t _writerIndex = INVALID_VALUE32;

            /*
            * Which scopes that write has already been published to, one bit per VkmPipelineScope.
            * A G-buffer sampled by four graphics passes in a row must cost one barrier, not four --
            * but publishing to a draw does not make the write visible to a later dispatch, so the
            * scope has to be part of the answer rather than a single "already published" flag.
            */
            uint8_t _publishedScopes = 0;

            /*
            * Every read since that write, not just the latest. A write-after-read has to wait for
            * all of them: two subgraphs sampling one texture may still both be in flight, and a
            * barrier naming only the later one leaves the earlier unordered. Kept as a list rather
            * than a union because VkmResourceAccess is a closed enum with nothing to OR.
            */
            struct Reader
            {
                VkmResourceAccess _access = VkmResourceAccess::None;
                VkmPipelineScope _scope = VkmPipelineScope::All;
                uint32_t _index = INVALID_VALUE32;
            };
            std::vector<Reader> _readers;

            LayoutClass _layout = LayoutClass::None;
        };

        struct SubresourceKey
        {
            VkmResourceHandle _handle;
            uint32_t _subresource; // layer * mipCount + mip

            bool operator==(const SubresourceKey& other) const
            {
                return _handle == other._handle && _subresource == other._subresource;
            }
        };

        struct SubresourceKeyHash
        {
            size_t operator()(const SubresourceKey& key) const
            {
                return std::hash<VkmResourceHandle>{}(key._handle) ^
                       (static_cast<size_t>(key._subresource) * 0x9E3779B97F4A7C15ull);
            }
        };

        // Resolves VKM_ALL_REMAINING_SUBRESOURCES against what the resource actually has.
        void resolveRange(const VkmSubresourceRange& range, uint32_t mipCount, uint32_t layerCount,
                          uint32_t* outFirstMip, uint32_t* outLastMip,
                          uint32_t* outFirstLayer, uint32_t* outLastLayer)
        {
            *outFirstMip = std::min(range._baseMipLevel, mipCount - 1u);
            *outFirstLayer = std::min(range._baseArrayLayer, layerCount - 1u);
            *outLastMip = (range._mipLevelCount == VKM_ALL_REMAINING_SUBRESOURCES)
                              ? (mipCount - 1u)
                              : std::min(*outFirstMip + range._mipLevelCount - 1u, mipCount - 1u);
            *outLastLayer = (range._arrayLayerCount == VKM_ALL_REMAINING_SUBRESOURCES)
                                ? (layerCount - 1u)
                                : std::min(*outFirstLayer + range._arrayLayerCount - 1u, layerCount - 1u);
        }
    } // namespace

    VkmPipelineScope vkmPipelineScopeOfSubGraph(VkmRenderSubGraphType type)
    {
        switch (type)
        {
            case VkmRenderSubGraphType::Graphics: return VkmPipelineScope::Graphics;
            case VkmRenderSubGraphType::Compute:  return VkmPipelineScope::Compute;
            case VkmRenderSubGraphType::Transfer: return VkmPipelineScope::Transfer;
            default:                             return VkmPipelineScope::All;
        }
    }

    VkmRenderGraphBarrierPlan vkmBuildRenderGraphBarrierPlan(const VkmSubGraphAccessView* subGraphs,
                                                             uint32_t subGraphCount,
                                                             const VkmResourceSubresourceLookup& lookup,
                                                             bool optimize, bool validate)
    {
        VkmRenderGraphBarrierPlan plan;
        plan._acquire.resize(subGraphCount);
        plan._release.resize(subGraphCount);
        plan._dependencies.resize(subGraphCount);
        if (subGraphs == nullptr || subGraphCount == 0)
        {
            return plan;
        }

        std::unordered_map<SubresourceKey, SubresourceState, SubresourceKeyHash> tracker;

        for (uint32_t index = 0; index < subGraphCount; ++index)
        {
            const VkmSubGraphAccessView& subGraph = subGraphs[index];

            for (uint32_t accessIndex = 0; accessIndex < subGraph._accessCount; ++accessIndex)
            {
                const VkmSubGraphAccess& declared = subGraph._accesses[accessIndex];
                if (declared._access == VkmResourceAccess::None || !declared._handle.isValid())
                {
                    // A "keep this alive, it participates in no hazard" declaration -- a sampler,
                    // or a handle held only so the deferred reclaimer waits for the frame.
                    continue;
                }

                uint32_t mipCount = 1;
                uint32_t layerCount = 1;
                if (!lookup.getSubresourceCounts(declared._handle, &mipCount, &layerCount))
                {
                    if (validate)
                    {
                        plan._validationErrors.push_back(
                            "SubGraph#" + std::to_string(subGraph._subGraphId) + " declares " +
                            vkmResourceAccessName(declared._access) +
                            " on a handle that names no live resource");
                    }
                    continue;
                }

                uint32_t firstMip = 0, lastMip = 0, firstLayer = 0, lastLayer = 0;
                resolveRange(declared._range, mipCount, layerCount, &firstMip, &lastMip, &firstLayer,
                             &lastLayer);

                const bool writes = vkmIsWriteAccess(declared._access);
                const LayoutClass wantedLayout =
                    hasLayout(declared._handle) ? layoutClassOf(declared._access) : LayoutClass::None;

                for (uint32_t layer = firstLayer; layer <= lastLayer; ++layer)
                {
                    for (uint32_t mip = firstMip; mip <= lastMip; ++mip)
                    {
                        const SubresourceKey key{ declared._handle, layer * mipCount + mip };
                        SubresourceState& state = tracker[key];

                        const bool priorWrite = state._writerIndex != INVALID_VALUE32;
                        const bool layoutChange = wantedLayout != state._layout;

                        // Records one half-pair into the plan and notes the dependency edge.
                        const auto emit = [&](VkmResourceAccess srcAccess, VkmPipelineScope srcScope,
                                              uint32_t producer, bool executionOnly) {
                            /*
                            * A subgraph that declares one resource twice is its own prior access on
                            * the second declaration. Both halves are recorded before that subgraph
                            * runs, so such a barrier could only order it against itself. Two
                            * declarations are how a subgraph says it touches something two ways --
                            * the scene's cull pass clears its visible list and then
                            * read-modify-writes it -- and how an attachment a caller also declared
                            * by hand meets the one derived from the frame buffer descriptor.
                            */
                            if (producer == index)
                            {
                                if (validate && layoutChange)
                                {
                                    plan._validationErrors.push_back(
                                        "SubGraph#" + std::to_string(subGraph._subGraphId) +
                                        " declares " + vkmResourceAccessName(declared._access) +
                                        " on a subresource it already declared in a different image"
                                        " layout; no barrier can satisfy both");
                                }
                                return;
                            }

                            VkmResourceBarrier barrier{};
                            barrier._handle = declared._handle;
                            barrier._srcAccess = srcAccess;
                            barrier._srcScope = srcScope;
                            barrier._dstAccess = declared._access;
                            barrier._dstScope = subGraph._scope;
                            barrier._range = VkmSubresourceRange{ mip, 1, layer, 1 };
                            barrier._executionOnly = executionOnly;
                            plan._acquire[index].push_back(barrier);

                            if (producer == INVALID_VALUE32)
                            {
                                return; // produced outside this graph: only the backend knows by what
                            }
                            /*
                            * Every dependency gets its release half, adjacent or not. Whether a
                            * split is worth taking is a backend decision -- Vulkan folds the
                            * release into the acquire that consumes it, so for it an adjacent pair
                            * costs nothing either way -- and a backend that narrows its
                            * encoder-boundary barriers from this list needs the list to be
                            * complete. Dropping the adjacent case here would silently omit the
                            * nearest dependency, which is the one most likely to matter.
                            */
                            plan._release[producer].push_back(barrier);

                            std::vector<uint32_t>& dependencies = plan._dependencies[index];
                            const uint32_t producerId = subGraphs[producer]._subGraphId;
                            if (std::find(dependencies.begin(), dependencies.end(), producerId) ==
                                dependencies.end())
                            {
                                dependencies.push_back(producerId);
                            }
                        };

                        if (writes)
                        {
                            // Write-after-read waits for *every* reader since the last write, not
                            // just the latest -- they may all still be in flight. Each is
                            // execution-only: a read publishes nothing that needs flushing.
                            for (const SubresourceState::Reader& reader : state._readers)
                            {
                                emit(reader._access, reader._scope, reader._index, /*executionOnly=*/true);
                            }
                            // Write-after-write, or a first write that still needs its layout set.
                            if (priorWrite || layoutChange || !optimize)
                            {
                                emit(state._writeAccess, state._writeScope, state._writerIndex,
                                     /*executionOnly=*/false);
                            }

                            state._writeAccess = declared._access;
                            state._writeScope = subGraph._scope;
                            state._writerIndex = index;
                            state._publishedScopes = 0;
                            state._readers.clear();
                        }
                        else
                        {
                            // Read-after-read at one layout, already published to this scope, is
                            // the case that must cost nothing: a G-buffer sampled by four passes
                            // in a row is one barrier, not four.
                            const uint8_t scopeBit =
                                static_cast<uint8_t>(1u << static_cast<uint32_t>(subGraph._scope));
                            const bool alreadyPublished = (state._publishedScopes & scopeBit) != 0;
                            if ((priorWrite && !alreadyPublished) || layoutChange || !optimize)
                            {
                                emit(state._writeAccess, state._writeScope, state._writerIndex,
                                     /*executionOnly=*/false);
                            }
                            // A layout change re-publishes: the transition itself is the barrier
                            // every later reader now sits behind.
                            state._publishedScopes = layoutChange ? scopeBit
                                                                  : static_cast<uint8_t>(state._publishedScopes | scopeBit);
                            state._readers.push_back(
                                SubresourceState::Reader{ declared._access, subGraph._scope, index });
                        }
                        state._layout = wantedLayout;
                    }
                }
            }
        }

        return plan;
    }
}
