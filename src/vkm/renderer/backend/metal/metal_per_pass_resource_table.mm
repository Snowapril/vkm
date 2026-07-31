// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_per_pass_resource_table.h>

#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/metal/metal_buffer.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_sampler.h>
#include <vkm/renderer/backend/metal/metal_texture.h>

#import <Metal/MTL4ArgumentTable.h>
#import <Metal/MTLBuffer.h>
#import <Metal/MTLSampler.h>
#import <Metal/MTLTexture.h>

namespace vkm
{
    namespace
    {
        void setError(std::string* outError, const std::string& message)
        {
            VKM_DEBUG_ERROR(message.c_str());
            if (outError != nullptr && outError->empty())
            {
                *outError = message;
            }
        }
    }

    VkmPerPassResourceTableMetal::VkmPerPassResourceTableMetal(VkmDriverBase* driver)
        : VkmPerPassResourceTableBase(driver)
    {
    }

    VkmPerPassResourceTableMetal::~VkmPerPassResourceTableMetal()
    {
        destroyInner();
    }

    bool VkmPerPassResourceTableMetal::createInner(const std::vector<VkmPerPassResourceEntry>& entries,
                                                   std::string* outError)
    {
        const std::vector<VkmPerPassResourceBinding>& declaration =
            _pipelineState->getDescriptor().perPassResources;
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();

        _bindings.clear();
        _bindings.reserve(entries.size());

        // `entries` arrives in declaration order (the base class reorders it), so the two walk in
        // lockstep. The Metal indices must match the ones vkm-compiler pinned via
        // add_msl_resource_binding, which uses these same bases.
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const VkmPerPassResourceEntry& entry = entries[i];
            const VkmPerPassResourceBinding& declared = declaration[i];

            ResolvedBinding resolved{};
            switch (declared.type)
            {
                case VkmPerPassResourceType::SampledTexture:
                {
                    VkmTextureMetal* textureMetal = static_cast<VkmTextureMetal*>(
                        renderResourcePool->getResource<VkmTexture>(entry.resource));
                    if (textureMetal == nullptr)
                    {
                        setError(outError, "Binding " + std::to_string(declared.binding) +
                                               " was given a handle that is not a live texture");
                        return false;
                    }
                    resolved.kind = BindingKind::Texture;
                    resolved.index = kVkmMetalPerPassTextureIndexBase + declared.binding;
                    resolved.resourceId = [textureMetal->getInternalHandle() gpuResourceID];
                    break;
                }
                case VkmPerPassResourceType::Sampler:
                {
                    VkmSamplerMetal* samplerMetal = static_cast<VkmSamplerMetal*>(
                        renderResourcePool->getResource<VkmSampler>(entry.resource));
                    if (samplerMetal == nullptr)
                    {
                        setError(outError, "Binding " + std::to_string(declared.binding) +
                                               " was given a handle that is not a live sampler");
                        return false;
                    }
                    resolved.kind = BindingKind::SamplerState;
                    resolved.index = kVkmMetalPerPassSamplerIndexBase + declared.binding;
                    resolved.resourceId = [samplerMetal->getSampler() gpuResourceID];
                    break;
                }
                case VkmPerPassResourceType::StorageBuffer:
                case VkmPerPassResourceType::UniformBuffer:
                {
                    VkmBufferMetal* bufferMetal = static_cast<VkmBufferMetal*>(
                        renderResourcePool->getResource<VkmBuffer>(entry.resource));
                    if (bufferMetal == nullptr)
                    {
                        setError(outError, "Binding " + std::to_string(declared.binding) +
                                               " was given a handle that is not a live buffer");
                        return false;
                    }
                    resolved.kind = BindingKind::Address;
                    resolved.index = kVkmMetalPerPassBufferIndexBase + declared.binding;
                    resolved.address = bufferMetal->getBuffer().gpuAddress;
                    break;
                }
            }
            _bindings.push_back(resolved);
        }
        return true;
    }

    void VkmPerPassResourceTableMetal::applyTo(id<MTL4ArgumentTable> argumentTable) const
    {
        for (const ResolvedBinding& binding : _bindings)
        {
            switch (binding.kind)
            {
                case BindingKind::Address:
                    [argumentTable setAddress:binding.address atIndex:binding.index];
                    break;
                case BindingKind::Texture:
                    [argumentTable setTexture:binding.resourceId atIndex:binding.index];
                    break;
                case BindingKind::SamplerState:
                    [argumentTable setSamplerState:binding.resourceId atIndex:binding.index];
                    break;
            }
        }
    }

    void VkmPerPassResourceTableMetal::destroyInner()
    {
        // Nothing owned: the addresses and resource IDs belong to the resources themselves, which
        // outlive this table by the caller's contract.
        _bindings.clear();
    }
} // namespace vkm
