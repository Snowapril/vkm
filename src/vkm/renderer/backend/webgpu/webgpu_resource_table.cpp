// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_resource_table.h>

#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/webgpu/webgpu_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_pipeline_state.h>
#include <vkm/renderer/backend/webgpu/webgpu_sampler.h>
#include <vkm/renderer/backend/webgpu/webgpu_texture.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>

#include <vector>

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

    VkmResourceTableWebGPU::VkmResourceTableWebGPU(VkmDriverBase* driver)
        : VkmResourceTableBase(driver)
    {
    }

    VkmResourceTableWebGPU::~VkmResourceTableWebGPU()
    {
        destroyInner();
    }

    bool VkmResourceTableWebGPU::createInner(const std::vector<VkmTableResourceEntry>& entries,
                                                    std::string* outError)
    {
        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);
        WGPUDevice device = driverWebGPU->getDevice();

        const VkmPipelineStateWebGPU* pipelineStateWebGPU =
            static_cast<const VkmPipelineStateWebGPU*>(_pipelineState);
        WGPUBindGroupLayout layout = pipelineStateWebGPU->getBindGroupLayout(getSetKind());
        if (layout == nullptr)
        {
            setError(outError, "Pipeline '" + _pipelineState->getName() + "' has no group-" +
                                   std::to_string(getSetIndex()) + " layout");
            return false;
        }

        const std::vector<VkmTableResourceBinding>& declaration = getDeclaration();
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();

        std::vector<WGPUBindGroupEntry> bindEntries;
        bindEntries.reserve(entries.size());
        _textureViews.reserve(entries.size());

        // `entries` arrives in declaration order (the base class reorders it), so the two walk in
        // lockstep.
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const VkmTableResourceEntry& entry = entries[i];
            const VkmTableResourceBinding& declared = declaration[i];

            WGPUBindGroupEntry bindEntry{};
            bindEntry.binding = declared.binding;

            switch (declared.type)
            {
                case VkmTableResourceType::SampledTexture:
                {
                    VkmTextureWebGPU* textureWebGPU = static_cast<VkmTextureWebGPU*>(
                        renderResourcePool->getResource<VkmTexture>(entry.resource));
                    if (textureWebGPU == nullptr)
                    {
                        setError(outError, "Binding " + std::to_string(declared.binding) +
                                               " was given a handle that is not a live texture");
                        destroyInner();
                        return false;
                    }
                    // Unlike the render-pass path, which releases its views immediately, this view
                    // is referenced by the bind group for as long as the table lives.
                    WGPUTextureView view = wgpuTextureCreateView(textureWebGPU->getWGPUTexture(), nullptr);
                    _textureViews.push_back(view);
                    bindEntry.textureView = view;
                    break;
                }
                case VkmTableResourceType::Sampler:
                {
                    VkmSamplerWebGPU* samplerWebGPU = static_cast<VkmSamplerWebGPU*>(
                        renderResourcePool->getResource<VkmSampler>(entry.resource));
                    if (samplerWebGPU == nullptr)
                    {
                        setError(outError, "Binding " + std::to_string(declared.binding) +
                                               " was given a handle that is not a live sampler");
                        destroyInner();
                        return false;
                    }
                    bindEntry.sampler = samplerWebGPU->getSampler();
                    break;
                }
                case VkmTableResourceType::StorageBuffer:
                case VkmTableResourceType::UniformBuffer:
                {
                    VkmBufferWebGPU* bufferWebGPU = static_cast<VkmBufferWebGPU*>(
                        renderResourcePool->getResource<VkmBuffer>(entry.resource));
                    if (bufferWebGPU == nullptr)
                    {
                        setError(outError, "Binding " + std::to_string(declared.binding) +
                                               " was given a handle that is not a live buffer");
                        destroyInner();
                        return false;
                    }
                    bindEntry.buffer = bufferWebGPU->getBuffer();
                    bindEntry.offset = 0;
                    bindEntry.size = bufferWebGPU->getBufferInfo()._size;
                    break;
                }
            }
            bindEntries.push_back(bindEntry);
        }

        WGPUBindGroupDescriptor bindGroupDesc{};
        bindGroupDesc.label = toWGPUStringView("VkmPerPassBindGroup");
        bindGroupDesc.layout = layout;
        bindGroupDesc.entryCount = bindEntries.size();
        bindGroupDesc.entries = bindEntries.data();
        _bindGroup = wgpuDeviceCreateBindGroup(device, &bindGroupDesc);
        if (_bindGroup == nullptr)
        {
            setError(outError, "Failed to create the group-2 bind group for " + _pipelineState->getName());
            destroyInner();
            return false;
        }
        return true;
    }

    void VkmResourceTableWebGPU::destroyInner()
    {
        if (_bindGroup != nullptr)
        {
            wgpuBindGroupRelease(_bindGroup);
            _bindGroup = nullptr;
        }
        for (WGPUTextureView view : _textureViews)
        {
            wgpuTextureViewRelease(view);
        }
        _textureViews.clear();
    }
} // namespace vkm
