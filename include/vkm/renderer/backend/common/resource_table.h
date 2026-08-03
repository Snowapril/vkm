// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/pipeline_state.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmPipelineStateBase;

    // One resource bound at a binding the pipeline declared for this table's set. `binding` must
    // name a binding in the pipeline's `perPassResources` (set 2) or `perDrawResources` (set 3),
    // and `resource`'s type must match what was declared there (a texture for SampledTexture, a
    // sampler for Sampler, a buffer for the two buffer kinds) -- both are checked when the table
    // is built.
    struct VkmTableResourceEntry
    {
        uint32_t binding = 0;
        VkmResourceHandle resource;
    };

    /*
    * @brief The resources bound at one PSO-declared descriptor set -- set 2 (per-pass) or set 3
    * (per-draw).
    *
    * @details Sets 0 and 1 are engine-global, so every pipeline shares them and bind sites need no
    * per-PSO knowledge. Sets 2 and 3 are the genuinely per-PSO ones: a table is built against one
    * pipeline's declaration for one set kind, and can only be bound while a pipeline sharing that
    * declaration is bound.
    *
    * The two kinds are one class rather than two hierarchies because they differ only by the set
    * index, by which declaration they validate against, and -- on Metal, which has no set index at
    * all -- by which argument-table index bases they occupy. Everything else is shared.
    *
    * A table is **immutable**: its resources are fixed when it is built. That is deliberate rather
    * than a limitation to lift later --
    *   - it removes the update-while-in-flight hazard entirely, so there is no need for
    *     FRAME_COUNT copies and no rule for callers to get wrong, and
    *   - the cases that "change" do not really want mutation anyway: a resized G-buffer wants a
    *     rebuilt table (the textures themselves were recreated), and a ping-ponged pair of
    *     reservoir buffers wants two tables selected by parity, which reads better than one table
    *     rewritten every frame.
    * Rebuilding a table is cheap; it is a descriptor-set write, an argument-buffer fill or a bind
    * group, not an allocation of the underlying resources.
    *
    * Owned by whoever created it, like VkmPipelineStateBase -- destroy() then delete. It must
    * outlive every in-flight frame that bound it, so release it the way any other GPU-referenced
    * object is released rather than at the end of a frame that may still be executing.
    */
    class VkmResourceTableBase
    {
    public:
        explicit VkmResourceTableBase(VkmDriverBase* driver);
        virtual ~VkmResourceTableBase();

        /*
        * @brief Validates `entries` against `pipelineState`'s declaration for `kind` and builds the
        * backend object.
        * @details Every declared binding must be supplied exactly once, with a resource of the
        * declared kind, and nothing may be supplied that was not declared. A partially populated
        * set is a validation error on Vulkan and undefined elsewhere, so it is rejected here where
        * the message can name the binding.
        */
        bool initialize(const VkmPipelineStateBase* pipelineState, VkmResourceSetKind kind,
                        const std::vector<VkmTableResourceEntry>& entries,
                        std::string* outError = nullptr);
        void destroy();

        // The pipeline whose declaration this table was built against, and which set it fills.
        inline const VkmPipelineStateBase* getPipelineState() const { return _pipelineState; }
        inline VkmResourceSetKind getSetKind() const { return _setKind; }
        inline uint32_t getSetIndex() const { return vkmResourceSetIndex(_setKind); }

        /*
        * @brief The declaration this table was built against.
        *
        * @details Bind-time compatibility is checked against *this*, not against the pipeline
        * pointer: a PSO with several permutations (the G-buffer's three vertex layouts, say)
        * produces distinct pipeline objects sharing one set-3 declaration, and requiring a table
        * per permutation would multiply the per-material tables by the permutation count for no
        * reason. Two pipelines with equal declarations have layout-compatible sets by construction.
        */
        const std::vector<VkmTableResourceBinding>& getDeclaration() const;

    protected:
        // `entries` has already been validated against the pipeline's declaration and reordered to
        // match it, so a backend can walk the declaration and this array in lockstep.
        virtual bool createInner(const std::vector<VkmTableResourceEntry>& entries, std::string* outError) = 0;
        virtual void destroyInner() = 0;

    protected:
        VkmDriverBase* _driver;
        const VkmPipelineStateBase* _pipelineState = nullptr;
        VkmResourceSetKind _setKind = VkmResourceSetKind::PerPass;
    };
} // namespace vkm
