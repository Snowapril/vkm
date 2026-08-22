// Copyright (c) 2026 Snowapril

#include <vkm/renderer/scene/scene_material_tables.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/scene/scene.h>

namespace vkm
{
    namespace
    {
        bool fail(std::string* outError, const std::string& message)
        {
            VKM_DEBUG_ERROR(message.c_str());
            if (outError != nullptr)
            {
                *outError = message;
            }
            return false;
        }
    } // namespace

    VkmSceneMaterialTables::~VkmSceneMaterialTables()
    {
        VKM_ASSERT(_tables.empty(), "VkmSceneMaterialTables::destroy must be called before the owner goes away");
    }

    bool VkmSceneMaterialTables::initialize(VkmDriverBase* driver, const VkmScene& scene,
                                            const VkmPipelineStateBase* pipeline, std::string* outError)
    {
        VKM_ASSERT(driver != nullptr && pipeline != nullptr, "VkmSceneMaterialTables needs a driver and a pipeline");

        // The pipeline's declaration is the whole test: a PSO scopes per_draw_resources to the
        // backends whose shader declares set 3, so an empty one means this backend samples
        // materials through the bindless array and wants no tables at all.
        if (pipeline->getDescriptor().perDrawResources.empty())
        {
            return true;
        }

        // One texel of white, for every channel a material has no texture for. glTF multiplies
        // factor by texture, so this leaves the factor untouched -- matching what the bindless path
        // does for an invalid slot. UNORM rather than SRGB because white is 1.0 in both.
        VkmTextureInfo placeholderInfo{};
        placeholderInfo._flags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst));
        placeholderInfo._extent = glm::uvec3(1, 1, 1);
        placeholderInfo._numMipLevels = 1;
        placeholderInfo._numArrayLayers = 1;
        placeholderInfo._format = VkmFormat::R8G8B8A8_UNORM;
        placeholderInfo._debugName = "SceneMaterialPlaceholder";

        VkmTexture* placeholder = driver->newTexture(placeholderInfo);
        if (placeholder == nullptr)
        {
            return fail(outError, "Failed to create the material placeholder texture");
        }
        _placeholderTexture = placeholder->getHandle();

        const uint8_t whiteTexel[4] = { 255, 255, 255, 255 };
        if (!driver->uploadToTexture(_placeholderTexture, whiteTexel, sizeof(whiteTexel)))
        {
            destroy(driver);
            return fail(outError, "Failed to upload the material placeholder texture");
        }

        // Repeat, matching the engine's set-0 sampler: glTF's default wrap, and the assets that
        // need per-draw tables are the same assets that rely on it.
        VkmSamplerInfo samplerInfo{};
        samplerInfo._addressModeU = VkmAddressMode::Repeat;
        samplerInfo._addressModeV = VkmAddressMode::Repeat;
        samplerInfo._addressModeW = VkmAddressMode::Repeat;
        samplerInfo._debugName = "SceneMaterialSampler";
        VkmSampler* sampler = driver->newSampler(samplerInfo);
        if (sampler == nullptr)
        {
            destroy(driver);
            return fail(outError, "Failed to create the material sampler");
        }
        _sampler = sampler->getHandle();

        const uint32_t materialCount = scene.getMaterialCount();
        _tables.reserve(materialCount);
        for (uint32_t i = 0; i < materialCount; ++i)
        {
            const VkmScene::MaterialTextures textures = scene.getMaterialTextures(i);
            const VkmResourceHandle baseColor =
                textures._baseColor.isValid() ? textures._baseColor : _placeholderTexture;
            const VkmResourceHandle metallicRoughness =
                textures._metallicRoughness.isValid() ? textures._metallicRoughness : _placeholderTexture;
            // The white placeholder samples to 1, leaving the emissive factor exactly -- the same
            // absent-texture behaviour the other channels get.
            const VkmResourceHandle emissive =
                textures._emissive.isValid() ? textures._emissive : _placeholderTexture;

            // Binding order mirrors VKM_MATERIAL_DECLARE()'s WebGPU branch and the PSO's
            // per_draw_resources array; the three have to agree.
            std::string tableError;
            VkmResourceTableBase* table = driver->newResourceTable(
                pipeline, VkmResourceSetKind::PerDraw,
                {{ 0, baseColor }, { 1, metallicRoughness }, { 2, _sampler }, { 3, emissive }}, &tableError);
            if (table == nullptr)
            {
                destroy(driver);
                return fail(outError, "Failed to build the set-3 table for material " +
                                          std::to_string(i) + ": " + tableError);
            }
            _tables.push_back(table);
        }
        return true;
    }

    void VkmSceneMaterialTables::destroy(VkmDriverBase* driver)
    {
        for (VkmResourceTableBase* table : _tables)
        {
            if (table != nullptr)
            {
                table->destroy();
                delete table;
            }
        }
        _tables.clear();

        VkmRenderResourcePool* resourcePool = (driver != nullptr) ? driver->getRenderResourcePool() : nullptr;
        if (resourcePool != nullptr)
        {
            for (VkmResourceHandle* handle : { &_sampler, &_placeholderTexture })
            {
                if (handle->isValid())
                {
                    resourcePool->releaseResource(*handle);
                    *handle = VKM_INVALID_RESOURCE_HANDLE;
                }
            }
        }
    }

    void VkmSceneMaterialTables::bind(VkmCommandBufferBase* commandBuffer, uint32_t materialIndex) const
    {
        if (materialIndex < _tables.size())
        {
            commandBuffer->bindResourceTable(_tables[materialIndex]);
        }
    }
} // namespace vkm
