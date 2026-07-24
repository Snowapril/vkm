// Copyright (c) 2025 Snowapril

#include <vkm/renderer/scene/gltf_importer.h>

#include <vkm/base/common.h>

#include <cgltf/cgltf.h>
#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <meshoptimizer.h>

#include <utility>

namespace vkm
{
    namespace
    {
        const char* toString(cgltf_result result)
        {
            switch (result)
            {
                case cgltf_result_success:         return "success";
                case cgltf_result_data_too_short:  return "data too short";
                case cgltf_result_unknown_format:  return "unknown format";
                case cgltf_result_invalid_json:    return "invalid json";
                case cgltf_result_invalid_gltf:    return "invalid gltf";
                case cgltf_result_invalid_options: return "invalid options";
                case cgltf_result_file_not_found:  return "file not found";
                case cgltf_result_io_error:        return "io error";
                case cgltf_result_out_of_memory:   return "out of memory";
                case cgltf_result_legacy_gltf:     return "legacy gltf (1.0 is not supported)";
                default:                           return "unknown error";
            }
        }

        bool fail(std::string* outError, std::string message)
        {
            if (outError != nullptr)
            {
                *outError = std::move(message);
            }
            return false;
        }

        // Reads `componentCount` floats of element `index`, zero-filling when the accessor
        // is absent or unreadable (a sparse/corrupt accessor must not leave garbage behind).
        void readElement(const cgltf_accessor* accessor, cgltf_size index, float* out, cgltf_size componentCount)
        {
            if (accessor == nullptr || !cgltf_accessor_read_float(accessor, index, out, componentCount))
            {
                for (cgltf_size i = 0; i < componentCount; ++i)
                {
                    out[i] = 0.0f;
                }
            }
        }

        /*
        * Fills in normals for a primitive that ships without them. The glTF spec calls for
        * flat (per-face) normals here, which would require splitting every shared vertex;
        * we instead accumulate area-weighted face normals per vertex, which keeps the index
        * buffer intact at the cost of smoothing hard edges on such assets.
        */
        void generateNormals(VkmSceneMesh& mesh)
        {
            for (VkmSceneVertex& vertex : mesh._vertices)
            {
                vertex._normal[0] = 0.0f;
                vertex._normal[1] = 0.0f;
                vertex._normal[2] = 0.0f;
            }

            for (size_t i = 0; i + 2 < mesh._indices.size(); i += 3)
            {
                const uint32_t i0 = mesh._indices[i];
                const uint32_t i1 = mesh._indices[i + 1];
                const uint32_t i2 = mesh._indices[i + 2];

                const glm::vec3 p0 = glm::make_vec3(mesh._vertices[i0]._position);
                const glm::vec3 p1 = glm::make_vec3(mesh._vertices[i1]._position);
                const glm::vec3 p2 = glm::make_vec3(mesh._vertices[i2]._position);

                // Unnormalized: its length is twice the triangle area, which is exactly the
                // weight we want each face to contribute.
                const glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);

                for (const uint32_t index : { i0, i1, i2 })
                {
                    mesh._vertices[index]._normal[0] += faceNormal.x;
                    mesh._vertices[index]._normal[1] += faceNormal.y;
                    mesh._vertices[index]._normal[2] += faceNormal.z;
                }
            }

            for (VkmSceneVertex& vertex : mesh._vertices)
            {
                const glm::vec3 normal = glm::make_vec3(vertex._normal);
                const float length = glm::length(normal);
                const glm::vec3 unitNormal = length > 0.0f ? normal / length : glm::vec3(0.0f, 1.0f, 0.0f);
                vertex._normal[0] = unitNormal.x;
                vertex._normal[1] = unitNormal.y;
                vertex._normal[2] = unitNormal.z;
            }
        }

        void optimizeMesh(VkmSceneMesh& mesh)
        {
            if (mesh._indices.empty() || mesh._vertices.empty())
            {
                return;
            }

            meshopt_optimizeVertexCache(mesh._indices.data(), mesh._indices.data(),
                                        mesh._indices.size(), mesh._vertices.size());

            const size_t remainingVertexCount = meshopt_optimizeVertexFetch(
                mesh._vertices.data(), mesh._indices.data(), mesh._indices.size(),
                mesh._vertices.data(), mesh._vertices.size(), sizeof(VkmSceneVertex));
            mesh._vertices.resize(remainingVertexCount);
        }

        // Converts one triangle primitive; returns false (with a warning already logged) for
        // primitives the engine cannot draw, which are skipped rather than failing the import.
        bool convertPrimitive(const cgltf_data* data,
                              const cgltf_primitive& primitive,
                              const VkmGltfImportOptions& options,
                              VkmSceneMesh* outMesh)
        {
            if (primitive.type != cgltf_primitive_type_triangles)
            {
                VKM_DEBUG_WARN("glTF import: skipping a non-triangle primitive");
                return false;
            }

            const cgltf_accessor* positions = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
            if (positions == nullptr || positions->count == 0)
            {
                VKM_DEBUG_WARN("glTF import: skipping a primitive without POSITION data");
                return false;
            }

            const cgltf_accessor* normals = cgltf_find_accessor(&primitive, cgltf_attribute_type_normal, 0);
            const cgltf_accessor* uvs = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 0);
            const cgltf_accessor* tangents = cgltf_find_accessor(&primitive, cgltf_attribute_type_tangent, 0);

            VkmSceneMesh mesh;
            mesh._vertices.resize(positions->count);
            for (cgltf_size i = 0; i < positions->count; ++i)
            {
                VkmSceneVertex& vertex = mesh._vertices[i];
                vertex = VkmSceneVertex{};

                readElement(positions, i, vertex._position, 3);
                readElement(normals, i, vertex._normal, 3);
                readElement(uvs, i, vertex._uv0, 2);
                // Tangents stay zeroed when absent: nothing consumes them yet, and deriving
                // them properly needs a MikkTSpace-style generator.
                readElement(tangents, i, vertex._tangent, 4);

                mesh._bounds.expand(glm::make_vec3(vertex._position));
            }

            if (primitive.indices != nullptr)
            {
                mesh._indices.resize(primitive.indices->count);
                cgltf_accessor_unpack_indices(primitive.indices, mesh._indices.data(),
                                              sizeof(uint32_t), mesh._indices.size());

                for (const uint32_t index : mesh._indices)
                {
                    if (index >= mesh._vertices.size())
                    {
                        VKM_DEBUG_WARN("glTF import: skipping a primitive whose indices are out of range");
                        return false;
                    }
                }
            }
            else
            {
                mesh._indices.resize(positions->count);
                for (uint32_t i = 0; i < static_cast<uint32_t>(mesh._indices.size()); ++i)
                {
                    mesh._indices[i] = i;
                }
            }

            if (normals == nullptr)
            {
                generateNormals(mesh);
            }

            if (options._optimizeMeshes)
            {
                optimizeMesh(mesh);
            }

            mesh._materialIndex = primitive.material != nullptr
                                      ? static_cast<uint32_t>(cgltf_material_index(data, primitive.material))
                                      : INVALID_VALUE32;

            *outMesh = std::move(mesh);
            return true;
        }

        void convertMaterial(const cgltf_material& source, VkmSceneMaterial* outMaterial)
        {
            outMaterial->_name = source.name != nullptr ? source.name : "";
            outMaterial->_emissiveFactor = glm::make_vec3(source.emissive_factor);

            if (source.has_pbr_metallic_roughness)
            {
                const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
                outMaterial->_baseColorFactor = glm::make_vec4(pbr.base_color_factor);
                outMaterial->_metallicFactor = pbr.metallic_factor;
                outMaterial->_roughnessFactor = pbr.roughness_factor;
            }
        }
    } // namespace

    bool importGltfModel(const std::string& filePath,
                         VkmSceneModel* outModel,
                         std::string* outError,
                         const VkmGltfImportOptions& options)
    {
        if (outModel == nullptr)
        {
            return fail(outError, "outModel must not be null");
        }

        cgltf_options cgltfOptions{};
        cgltf_data* data = nullptr;

        cgltf_result result = cgltf_parse_file(&cgltfOptions, filePath.c_str(), &data);
        if (result != cgltf_result_success)
        {
            return fail(outError, "Failed to parse '" + filePath + "': " + toString(result));
        }

        result = cgltf_load_buffers(&cgltfOptions, data, filePath.c_str());
        if (result != cgltf_result_success)
        {
            cgltf_free(data);
            return fail(outError, "Failed to load buffers of '" + filePath + "': " + toString(result));
        }

        result = cgltf_validate(data);
        if (result != cgltf_result_success)
        {
            cgltf_free(data);
            return fail(outError, "Failed to validate '" + filePath + "': " + toString(result));
        }

        VkmSceneModel model;

        model._materials.resize(data->materials_count);
        for (cgltf_size i = 0; i < data->materials_count; ++i)
        {
            convertMaterial(data->materials[i], &model._materials[i]);
        }

        // glTF meshes are containers of primitives; the engine draws one primitive at a
        // time, so remember which scene meshes each glTF mesh expanded into.
        std::vector<std::vector<uint32_t>> primitiveIndicesPerMesh(data->meshes_count);
        for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex)
        {
            const cgltf_mesh& sourceMesh = data->meshes[meshIndex];
            for (cgltf_size primitiveIndex = 0; primitiveIndex < sourceMesh.primitives_count; ++primitiveIndex)
            {
                VkmSceneMesh mesh;
                if (!convertPrimitive(data, sourceMesh.primitives[primitiveIndex], options, &mesh))
                {
                    continue;
                }
                primitiveIndicesPerMesh[meshIndex].push_back(static_cast<uint32_t>(model._meshes.size()));
                model._meshes.push_back(std::move(mesh));
            }
        }

        model._nodes.resize(data->nodes_count);
        for (cgltf_size i = 0; i < data->nodes_count; ++i)
        {
            const cgltf_node& sourceNode = data->nodes[i];
            VkmSceneNode& node = model._nodes[i];

            node._name = sourceNode.name != nullptr ? sourceNode.name : "";
            cgltf_node_transform_local(&sourceNode, glm::value_ptr(node._localTransform));

            node._childIndices.reserve(sourceNode.children_count);
            for (cgltf_size child = 0; child < sourceNode.children_count; ++child)
            {
                node._childIndices.push_back(static_cast<uint32_t>(cgltf_node_index(data, sourceNode.children[child])));
            }

            if (sourceNode.mesh != nullptr)
            {
                node._meshIndices = primitiveIndicesPerMesh[cgltf_mesh_index(data, sourceNode.mesh)];
            }
        }

        const cgltf_scene* scene = data->scene != nullptr
                                       ? data->scene
                                       : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
        if (scene != nullptr)
        {
            model._rootNodeIndices.reserve(scene->nodes_count);
            for (cgltf_size i = 0; i < scene->nodes_count; ++i)
            {
                model._rootNodeIndices.push_back(static_cast<uint32_t>(cgltf_node_index(data, scene->nodes[i])));
            }
        }
        else
        {
            // No scene declared: every parentless node is a root.
            for (cgltf_size i = 0; i < data->nodes_count; ++i)
            {
                if (data->nodes[i].parent == nullptr)
                {
                    model._rootNodeIndices.push_back(static_cast<uint32_t>(i));
                }
            }
        }

        cgltf_free(data);

        *outModel = std::move(model);
        return true;
    }
} // namespace vkm
