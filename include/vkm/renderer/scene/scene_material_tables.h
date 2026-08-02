// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#include <string>
#include <vector>

namespace vkm
{
    class VkmCommandBufferBase;
    class VkmDriverBase;
    class VkmPipelineStateBase;
    class VkmResourceTableBase;
    class VkmScene;

    /*
    * @brief One descriptor set 3 (per-draw) table per material, for a backend that cannot index a
    * bindless texture array.
    *
    * @details On Vulkan and Metal a shader reaches a material's textures through the set-0 bindless
    * array, using the slots in VkmMaterialData, and nothing here is built. WebGPU has no
    * array-of-handle type, so its shaders declare the textures at set 3 and the runtime binds one
    * table per material before each draw -- which works because VkmScene splits draw batches per
    * material, so every draw has exactly one.
    *
    * Whether tables are needed is read off the **pipeline's own declaration** rather than off a
    * capability flag: a PSO scopes `per_draw_resources` to the backends whose shader actually
    * declares set 3 (see VkmPipelineStateDescriptor::perDrawResourceBackends), so an empty
    * declaration is exactly "this backend does not want them". One source of truth, and it cannot
    * disagree with the shader.
    *
    * A channel the material has no texture for still needs a binding, because an unbound entry in a
    * declared set is a validation error rather than a silently absent one. Those bind a 1x1 white
    * placeholder this class owns, which samples to 1 and leaves the material's factor exactly --
    * the same result the bindless path produces for an invalid slot.
    */
    class VkmSceneMaterialTables
    {
    public:
        VkmSceneMaterialTables() = default;
        ~VkmSceneMaterialTables();

        VkmSceneMaterialTables(const VkmSceneMaterialTables&) = delete;
        VkmSceneMaterialTables& operator=(const VkmSceneMaterialTables&) = delete;

        /*
        * @brief Builds one table per material of `scene` against `pipeline`'s set-3 declaration.
        *
        * @details A no-op returning true when `pipeline` declares no per-draw resources, which is
        * every backend that samples materials bindlessly. `scene` must already be built, since the
        * textures it hands out are created there.
        */
        bool initialize(VkmDriverBase* driver, const VkmScene& scene, const VkmPipelineStateBase* pipeline,
                        std::string* outError = nullptr);
        void destroy(VkmDriverBase* driver);

        // True when this backend reaches materials without per-draw tables, so binding is a no-op.
        inline bool isEmpty() const { return _tables.empty(); }

        /*
        * @brief Binds `materialIndex`'s table, if this backend uses them.
        *
        * Call from VkmScene::recordDrawBatches' beforeDraw hook, which is the only point at which
        * per-draw state can be set -- it runs after the batch's pipeline is bound.
        */
        void bind(VkmCommandBufferBase* commandBuffer, uint32_t materialIndex) const;

    private:
        std::vector<VkmResourceTableBase*> _tables; // 1:1 with the scene's materials
        VkmResourceHandle _placeholderTexture{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _sampler{ VKM_INVALID_RESOURCE_HANDLE };
    };
} // namespace vkm
