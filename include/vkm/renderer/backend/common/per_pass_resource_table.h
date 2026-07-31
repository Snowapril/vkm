// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmPipelineStateBase;

    // One resource bound at a set-2 binding the pipeline declared. `binding` must name a binding
    // in the pipeline's `perPassResources`, and `resource`'s type must match what was declared
    // there (a texture for SampledTexture, a sampler for Sampler, a buffer for the two buffer
    // kinds) -- both are checked when the table is built.
    struct VkmPerPassResourceEntry
    {
        uint32_t binding = 0;
        VkmResourceHandle resource;
    };

    /*
    * @brief The resources bound at descriptor set 2 (per-pass) for one pass.
    *
    * @details Sets 0 and 1 are engine-global, so every pipeline shares them and bind sites need no
    * per-PSO knowledge. Set 2 is the first genuinely per-PSO set: a table is built against one
    * pipeline's declaration and can only be bound while a pipeline sharing that declaration is
    * bound.
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
    class VkmPerPassResourceTableBase
    {
    public:
        explicit VkmPerPassResourceTableBase(VkmDriverBase* driver);
        virtual ~VkmPerPassResourceTableBase();

        /*
        * @brief Validates `entries` against `pipelineState`'s set-2 declaration and builds the
        * backend object.
        * @details Every declared binding must be supplied exactly once, with a resource of the
        * declared kind, and nothing may be supplied that was not declared. A partially populated
        * set is a validation error on Vulkan and undefined elsewhere, so it is rejected here where
        * the message can name the binding.
        */
        bool initialize(const VkmPipelineStateBase* pipelineState,
                        const std::vector<VkmPerPassResourceEntry>& entries,
                        std::string* outError = nullptr);
        void destroy();

        // The pipeline whose declaration this table was built against. Binding it while a
        // pipeline with a different set-2 declaration is bound is a caller error.
        inline const VkmPipelineStateBase* getPipelineState() const { return _pipelineState; }

    protected:
        // `entries` has already been validated against the pipeline's declaration and reordered to
        // match it, so a backend can walk the declaration and this array in lockstep.
        virtual bool createInner(const std::vector<VkmPerPassResourceEntry>& entries, std::string* outError) = 0;
        virtual void destroyInner() = 0;

    protected:
        VkmDriverBase* _driver;
        const VkmPipelineStateBase* _pipelineState = nullptr;
    };
} // namespace vkm
