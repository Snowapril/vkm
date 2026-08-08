// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/scene/scene_model.h>

#include <string>

namespace vkm
{
    struct VkmGltfImportOptions
    {
        // Reorders indices/vertices with meshoptimizer for vertex-cache and vertex-fetch
        // locality. Disable to keep the file's original ordering (unit tests rely on it).
        bool _optimizeMeshes = true;

        // Which engine vertex layout every imported primitive is packed into. Attributes the
        // layout does not carry are simply dropped, so importing an asset as PositionOnly is
        // how a caller asks for geometry without shading data.
        VkmVertexLayoutPreset _vertexLayout = VkmVertexLayoutPreset::StandardPBR;
    };

    /*
    * @brief Import a glTF 2.0 asset, .gltf or .glb, into a VkmSceneModel.
    * @details Only triangle primitives are imported; other primitive modes are skipped with a
    * warning. Attributes are packed into the layout named by `options._vertexLayout`: missing
    * normals are generated when that layout carries a normal, and missing UVs and tangents are left
    * zeroed. Exception-free, emscripten builds compiling without -fexceptions, so every failure is
    * reported through the return value.
    * @return False on failure, leaving `outModel` untouched and filling `outError`.
    */
    bool importGltfModel(const std::string& filePath,
                         VkmSceneModel* outModel,
                         std::string* outError,
                         const VkmGltfImportOptions& options = VkmGltfImportOptions{});
} // namespace vkm
