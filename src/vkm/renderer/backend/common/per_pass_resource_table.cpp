// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/per_pass_resource_table.h>

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
        // uses it so adding a VkmPerPassResourceType forces a decision here rather than silently
        // accepting anything.
        bool resourceTypeMatches(VkmPerPassResourceType declared, VkmResourceType provided)
        {
            switch (declared)
            {
                case VkmPerPassResourceType::SampledTexture:
                    return provided == VkmResourceType::Texture;
                case VkmPerPassResourceType::Sampler:
                    return provided == VkmResourceType::Sampler;
                case VkmPerPassResourceType::StorageBuffer:
                case VkmPerPassResourceType::UniformBuffer:
                    return provided == VkmResourceType::Buffer;
            }
            return false;
        }

        const char* perPassResourceTypeName(VkmPerPassResourceType type)
        {
            switch (type)
            {
                case VkmPerPassResourceType::SampledTexture: return "sampled_texture";
                case VkmPerPassResourceType::Sampler:        return "sampler";
                case VkmPerPassResourceType::StorageBuffer:  return "storage_buffer";
                case VkmPerPassResourceType::UniformBuffer:  return "uniform_buffer";
            }
            return "unknown";
        }
    }

    VkmPerPassResourceTableBase::VkmPerPassResourceTableBase(VkmDriverBase* driver) : _driver(driver)
    {
    }

    VkmPerPassResourceTableBase::~VkmPerPassResourceTableBase()
    {
    }

    bool VkmPerPassResourceTableBase::initialize(const VkmPipelineStateBase* pipelineState,
                                                 const std::vector<VkmPerPassResourceEntry>& entries,
                                                 std::string* outError)
    {
        if (pipelineState == nullptr)
        {
            setError(outError, "A per-pass resource table needs a pipeline state to build against");
            return false;
        }

        const std::vector<VkmPerPassResourceBinding>& declaration =
            pipelineState->getDescriptor().perPassResources;
        if (declaration.empty())
        {
            setError(outError, "Pipeline '" + pipelineState->getName() +
                                   "' declares no per-pass resources, so it has no set 2 to fill");
            return false;
        }
        if (entries.size() != declaration.size())
        {
            setError(outError, "Pipeline '" + pipelineState->getName() + "' declares " +
                                   std::to_string(declaration.size()) + " per-pass resources but " +
                                   std::to_string(entries.size()) + " were supplied");
            return false;
        }

        // Reordered into declaration order so backends can walk the two in lockstep instead of
        // each re-deriving the mapping.
        std::vector<VkmPerPassResourceEntry> ordered;
        ordered.reserve(declaration.size());
        for (const VkmPerPassResourceBinding& declared : declaration)
        {
            const auto match = std::find_if(entries.begin(), entries.end(),
                [&declared](const VkmPerPassResourceEntry& entry) { return entry.binding == declared.binding; });
            if (match == entries.end())
            {
                setError(outError, "Pipeline '" + pipelineState->getName() + "' declares binding " +
                                       std::to_string(declared.binding) + " (" +
                                       perPassResourceTypeName(declared.type) + ") but no resource was supplied for it");
                return false;
            }
            if (!resourceTypeMatches(declared.type, match->resource.type))
            {
                setError(outError, "Binding " + std::to_string(declared.binding) + " of pipeline '" +
                                       pipelineState->getName() + "' is declared " +
                                       perPassResourceTypeName(declared.type) +
                                       " but was given a " + vkmResourceTypeName(match->resource.type));
                return false;
            }
            ordered.push_back(*match);
        }

        _pipelineState = pipelineState;
        if (!createInner(ordered, outError))
        {
            _pipelineState = nullptr;
            return false;
        }
        return true;
    }

    void VkmPerPassResourceTableBase::destroy()
    {
        destroyInner();
        _pipelineState = nullptr;
    }
} // namespace vkm
