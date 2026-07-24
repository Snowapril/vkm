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
    };

    /*
    * @brief Import a glTF 2.0 asset (.gltf or .glb) into a VkmSceneModel.
    *
    * Only triangle primitives are imported; other primitive modes are skipped with a
    * warning. Attributes are converted into the fixed VkmSceneVertex layout: missing
    * normals are generated, missing UVs/tangents are left zeroed.
    *
    * Exception-free by design (emscripten builds compile without -fexceptions): every
    * failure is reported through the return value and `outError`.
    *
    * @return true on success; false leaves `outModel` untouched and fills `outError`.
    */
    bool importGltfModel(const std::string& filePath,
                         VkmSceneModel* outModel,
                         std::string* outError,
                         const VkmGltfImportOptions& options = VkmGltfImportOptions{});
} // namespace vkm
