// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/metal/metal_resource_table.h>

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

    VkmResourceTableMetal::VkmResourceTableMetal(VkmDriverBase* driver)
        : VkmResourceTableBase(driver)
    {
    }

    VkmResourceTableMetal::~VkmResourceTableMetal()
    {
        destroyInner();
    }

    bool VkmResourceTableMetal::createInner(const std::vector<VkmTableResourceEntry>& entries,
                                                   std::string* outError)
    {
        const std::vector<VkmTableResourceBinding>& declaration = getDeclaration();
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();

        // Metal has no set index, so which set this table fills is carried entirely by these
        // indices -- get them wrong and set 3 silently aliases onto set 2. The assigner walks the
        // declaration in the same order vkm-compiler did when it pinned the shader's arguments.
        VkmMetalTableIndexAssigner indexAssigner(getSetKind());

        _bindings.clear();
        _bindings.reserve(entries.size());

        // `entries` arrives in declaration order (the base class reorders it), so the two walk in
        // lockstep. The Metal indices must match the ones vkm-compiler pinned via
        // add_msl_resource_binding, which uses these same bases.
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const VkmTableResourceEntry& entry = entries[i];
            const VkmTableResourceBinding& declared = declaration[i];

            ResolvedBinding resolved{};
            switch (declared.type)
            {
                case VkmTableResourceType::SampledTexture:
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
                    resolved.index = indexAssigner.next(declared.type);
                    resolved.resourceId = [textureMetal->getInternalHandle() gpuResourceID];
                    break;
                }
                case VkmTableResourceType::Sampler:
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
                    resolved.index = indexAssigner.next(declared.type);
                    resolved.resourceId = [samplerMetal->getSampler() gpuResourceID];
                    break;
                }
                case VkmTableResourceType::StorageBuffer:
                case VkmTableResourceType::UniformBuffer:
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
                    resolved.index = indexAssigner.next(declared.type);
                    resolved.address = bufferMetal->getBuffer().gpuAddress;
                    break;
                }
            }
            _bindings.push_back(resolved);
        }
        return true;
    }

    void VkmResourceTableMetal::applyTo(id<MTL4ArgumentTable> argumentTable) const
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

    void VkmResourceTableMetal::destroyInner()
    {
        // Nothing owned: the addresses and resource IDs belong to the resources themselves, which
        // outlive this table by the caller's contract.
        _bindings.clear();
    }
} // namespace vkm
