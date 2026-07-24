// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/scene/scene_model.h>

#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief GPU-side companion of a VkmSceneModel.
    *
    * Every mesh becomes one vertex buffer plus one index buffer registered into the
    * engine-global bindless set, so shaders pull both through slot indices passed as push
    * constants (the same convention the triangle sample uses).
    */
    class VkmSceneModelGpu
    {
    public:
        struct MeshGpu
        {
            VkmResourceHandle _vertexBuffer{ VKM_INVALID_RESOURCE_HANDLE };
            VkmResourceHandle _indexBuffer{ VKM_INVALID_RESOURCE_HANDLE };
            uint32_t _vertexBufferSlot = INVALID_VALUE32;
            uint32_t _indexBufferSlot = INVALID_VALUE32;
            uint32_t _indexCount = 0;
        };

        VkmSceneModelGpu() = default;
        ~VkmSceneModelGpu() = default;

        /*
        * @brief Create, fill and bindless-register the buffers of every mesh in `model`.
        * Blocking (uses VkmDriverBase::uploadToBuffer) -- call at setup time.
        * On failure everything uploaded so far is released again.
        */
        bool upload(VkmDriverBase* driver, const VkmSceneModel& model, std::string* outError);

        void destroy(VkmDriverBase* driver);

        inline const std::vector<MeshGpu>& getMeshes() const { return _meshes; }

    private:
        std::vector<MeshGpu> _meshes;
    };
} // namespace vkm
