// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/resource_table.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>

#include <algorithm>

namespace vkm
{
    namespace
    {
        void setError(std::string* outError, const std::string& message)
        {
            VKM_DEBUG_ERROR(message.c_str());
            if (outError != nullptr)
            {
                *outError = message;
            }
        }

        // The resource kind a declared binding must be given. Kept next to the validation that
        // uses it so adding a VkmTableResourceType forces a decision here rather than silently
        // accepting anything.
        bool resourceTypeMatches(VkmTableResourceType declared, VkmResourceType provided)
        {
            switch (declared)
            {
                case VkmTableResourceType::SampledTexture:
                    return provided == VkmResourceType::Texture;
                case VkmTableResourceType::Sampler:
                    return provided == VkmResourceType::Sampler;
                case VkmTableResourceType::StorageBuffer:
                case VkmTableResourceType::UniformBuffer:
                    return provided == VkmResourceType::Buffer;
            }
            return false;
        }

        const char* tableResourceTypeName(VkmTableResourceType type)
        {
            switch (type)
            {
                case VkmTableResourceType::SampledTexture: return "sampled_texture";
                case VkmTableResourceType::Sampler:        return "sampler";
                case VkmTableResourceType::StorageBuffer:  return "storage_buffer";
                case VkmTableResourceType::UniformBuffer:  return "uniform_buffer";
            }
            return "unknown";
        }
    }

    VkmResourceTableBase::VkmResourceTableBase(VkmDriverBase* driver) : _driver(driver)
    {
    }

    VkmResourceTableBase::~VkmResourceTableBase()
    {
    }

    const std::vector<VkmTableResourceBinding>& VkmResourceTableBase::getDeclaration() const
    {
        return _pipelineState->getDescriptor().resourcesFor(_setKind);
    }

    bool VkmResourceTableBase::initialize(const VkmPipelineStateBase* pipelineState,
                                          VkmResourceSetKind kind,
                                          const std::vector<VkmTableResourceEntry>& entries,
                                          std::string* outError)
    {
        const std::string kindName = vkmResourceSetKindName(kind);
        const std::string setName = "set " + std::to_string(vkmResourceSetIndex(kind));
        if (pipelineState == nullptr)
        {
            setError(outError, "A " + kindName + " resource table needs a pipeline state to build against");
            return false;
        }

        const std::vector<VkmTableResourceBinding>& declaration =
            pipelineState->getDescriptor().resourcesFor(kind);
        if (declaration.empty())
        {
            setError(outError, "Pipeline '" + pipelineState->getName() + "' declares no " + kindName +
                                   " resources, so it has no " + setName + " to fill");
            return false;
        }
        if (entries.size() != declaration.size())
        {
            setError(outError, "Pipeline '" + pipelineState->getName() + "' declares " +
                                   std::to_string(declaration.size()) + " " + kindName + " resources but " +
                                   std::to_string(entries.size()) + " were supplied");
            return false;
        }

        // Reordered into declaration order so backends can walk the two in lockstep instead of
        // each re-deriving the mapping.
        std::vector<VkmTableResourceEntry> ordered;
        ordered.reserve(declaration.size());
        for (const VkmTableResourceBinding& declared : declaration)
        {
            const auto match = std::find_if(entries.begin(), entries.end(),
                [&declared](const VkmTableResourceEntry& entry) { return entry.binding == declared.binding; });
            if (match == entries.end())
            {
                setError(outError, "Pipeline '" + pipelineState->getName() + "' declares binding " +
                                       std::to_string(declared.binding) + " (" +
                                       tableResourceTypeName(declared.type) + ") but no resource was supplied for it");
                return false;
            }
            if (!resourceTypeMatches(declared.type, match->resource.type))
            {
                setError(outError, "Binding " + std::to_string(declared.binding) + " of pipeline '" +
                                       pipelineState->getName() + "' is declared " +
                                       tableResourceTypeName(declared.type) +
                                       " but was given a " + vkmResourceTypeName(match->resource.type));
                return false;
            }
            ordered.push_back(*match);
        }

        _pipelineState = pipelineState;
        _setKind = kind;
        if (!createInner(ordered, outError))
        {
            _pipelineState = nullptr;
            return false;
        }
        return true;
    }

    void VkmResourceTableBase::destroy()
    {
        destroyInner();
        _pipelineState = nullptr;
    }
} // namespace vkm
