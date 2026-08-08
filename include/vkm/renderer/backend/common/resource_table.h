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

    /*
    * @brief One resource bound at a binding the pipeline declared for this table's set.
    * @details `binding` must name a binding in the pipeline's `perPassResources` (set 2) or
    * `perDrawResources` (set 3), and `resource`'s type must match what was declared there: a
    * texture for SampledTexture, a sampler for Sampler, a buffer for the two buffer kinds. Both
    * are checked when the table is built.
    */
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
    * per-PSO knowledge. Sets 2 and 3 are the per-PSO ones: a table is built against one pipeline's
    * declaration for one set kind, and can only be bound while a pipeline sharing that declaration
    * is bound. The two kinds are one class because they differ only by set index, by which
    * declaration they validate against, and on Metal by which argument-table index bases they
    * occupy.
    * A table is immutable -- its resources are fixed when it is built -- which removes the
    * update-while-in-flight hazard, so there is no need for FRAME_COUNT copies. A resized G-buffer
    * wants a rebuilt table anyway, its textures having been recreated, and a ping-ponged pair of
    * reservoir buffers wants two tables selected by parity. Rebuilding is a descriptor-set write,
    * an argument-buffer fill or a bind group, not an allocation of the underlying resources.
    * Owned by whoever created it, like VkmPipelineStateBase: destroy() then delete. It must outlive
    * every in-flight frame that bound it.
    */
    class VkmResourceTableBase
    {
    public:
        explicit VkmResourceTableBase(VkmDriverBase* driver);
        virtual ~VkmResourceTableBase();

        /*
        * @brief Validates entries against a pipeline's declaration and builds the backend object.
        * @details A partially populated set is a validation error on Vulkan and undefined
        * elsewhere, so it is rejected here where the message can name the binding.
        * @param pipelineState Pipeline whose declaration the entries must match.
        * @param kind Which set this table fills.
        * @param entries Every declared binding exactly once, each with a resource of the declared
        * kind, and nothing that was not declared.
        * @param outError Receives the offending binding and reason. May be null.
        * @return False when validation or backend creation failed.
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
        * @details Bind-time compatibility is checked against this, not against the pipeline
        * pointer: a PSO with several permutations produces distinct pipeline objects sharing one
        * set-3 declaration. Two pipelines with equal declarations have layout-compatible sets by
        * construction.
        */
        const std::vector<VkmTableResourceBinding>& getDeclaration() const;

        /*
        * @brief Appends what this table binds, tagged with how the shader accesses it, for
        * VkmRenderSubGraph::addReferencedResources.
        * @details A table is the one place a subgraph's resources are named without the render
        * graph seeing them, so a pass that binds one and does not declare its contents is a pass
        * the dependency analysis believes reads nothing. The access follows the declared binding
        * type: a sampled texture is a shader read, a constant buffer a uniform read, an
        * RWStructuredBuffer a shader read-write, and a sampler takes VkmResourceAccess::None
        * because it takes part in no hazard.
        * @param outDeclarations Receives one entry per binding. Not cleared first.
        */
        void collectReferencedResources(std::vector<VkmResourceAccessDeclaration>* outDeclarations) const;

    protected:
        // `entries` has already been validated against the pipeline's declaration and reordered to
        // match it, so a backend can walk the declaration and this array in lockstep.
        virtual bool createInner(const std::vector<VkmTableResourceEntry>& entries, std::string* outError) = 0;
        virtual void destroyInner() = 0;

    protected:
        VkmDriverBase* _driver;
        const VkmPipelineStateBase* _pipelineState = nullptr;
        VkmResourceSetKind _setKind = VkmResourceSetKind::PerPass;
        // In declaration order, so this and getDeclaration() index in lockstep.
        std::vector<VkmTableResourceEntry> _entries;
    };
} // namespace vkm
