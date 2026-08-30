// Copyright (c) 2026 Snowapril

#include <vkm/renderer/scene/gltf_importer.h>

#include <vkm/base/common.h>
#include <vkm/base/cpu_profiler.h>

#include <cgltf/cgltf.h>
#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <meshoptimizer.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

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
        // Byte address of vertex `index` inside a mesh's interleaved vertex storage.
        inline uint8_t* vertexAt(VkmSceneMesh& mesh, uint32_t index)
        {
            return mesh._vertexData.data() + static_cast<size_t>(index) * mesh._layout._stride;
        }
        inline const uint8_t* vertexAt(const VkmSceneMesh& mesh, uint32_t index)
        {
            return mesh._vertexData.data() + static_cast<size_t>(index) * mesh._layout._stride;
        }

        glm::vec3 readPosition(const VkmSceneMesh& mesh, uint32_t index)
        {
            float position[3];
            vkmReadVertexAttribute(vertexAt(mesh, index), mesh._layout, VkmVertexSemantic::Position, position, 3);
            return glm::make_vec3(position);
        }

        void generateNormals(VkmSceneMesh& mesh)
        {
            // Nothing to write into: a layout without a normal attribute (PositionOnly) simply
            // does not carry shading data.
            if (vkmFindVertexAttribute(mesh._layout, VkmVertexSemantic::Normal) == nullptr)
            {
                return;
            }

            // Accumulate in a float scratch rather than read-modify-writing the packed storage:
            // a quantized normal format would lose the running sum between triangles.
            std::vector<glm::vec3> accumulated(mesh._vertexCount, glm::vec3(0.0f));

            for (size_t i = 0; i + 2 < mesh._indices.size(); i += 3)
            {
                const uint32_t i0 = mesh._indices[i];
                const uint32_t i1 = mesh._indices[i + 1];
                const uint32_t i2 = mesh._indices[i + 2];

                // Unnormalized: its length is twice the triangle area, which is exactly the
                // weight we want each face to contribute.
                const glm::vec3 faceNormal =
                    glm::cross(readPosition(mesh, i1) - readPosition(mesh, i0),
                               readPosition(mesh, i2) - readPosition(mesh, i0));

                for (const uint32_t index : { i0, i1, i2 })
                {
                    accumulated[index] += faceNormal;
                }
            }

            for (uint32_t i = 0; i < mesh._vertexCount; ++i)
            {
                const float length = glm::length(accumulated[i]);
                const glm::vec3 unitNormal = length > 0.0f ? accumulated[i] / length : glm::vec3(0.0f, 1.0f, 0.0f);
                vkmWriteVertexAttribute(vertexAt(mesh, i), mesh._layout, VkmVertexSemantic::Normal,
                                        glm::value_ptr(unitNormal), 3);
            }
        }

        void optimizeMesh(VkmSceneMesh& mesh)
        {
            if (mesh._indices.empty() || mesh._vertexCount == 0)
            {
                return;
            }

            meshopt_optimizeVertexCache(mesh._indices.data(), mesh._indices.data(),
                                        mesh._indices.size(), mesh._vertexCount);

            // meshoptimizer takes the vertex stride, so this works for any layout preset.
            const size_t remainingVertexCount = meshopt_optimizeVertexFetch(
                mesh._vertexData.data(), mesh._indices.data(), mesh._indices.size(),
                mesh._vertexData.data(), mesh._vertexCount, mesh._layout._stride);
            mesh._vertexCount = static_cast<uint32_t>(remainingVertexCount);
            mesh._vertexData.resize(static_cast<size_t>(mesh._vertexCount) * mesh._layout._stride);
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
            mesh._layout = vkmGetVertexLayoutPreset(options._vertexLayout);
            mesh._vertexCount = static_cast<uint32_t>(positions->count);
            // Zero-initialized, which is what leaves attributes the asset omits at zero.
            mesh._vertexData.assign(static_cast<size_t>(mesh._vertexCount) * mesh._layout._stride, 0);

            for (cgltf_size i = 0; i < positions->count; ++i)
            {
                uint8_t* vertex = vertexAt(mesh, static_cast<uint32_t>(i));

                // Read through a float scratch and let vkmWriteVertexAttribute do the packing;
                // writes for semantics this layout omits are no-ops.
                float scratch[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

                readElement(positions, i, scratch, 3);
                vkmWriteVertexAttribute(vertex, mesh._layout, VkmVertexSemantic::Position, scratch, 3);
                mesh._bounds.expand(glm::make_vec3(scratch));

                readElement(normals, i, scratch, 3);
                vkmWriteVertexAttribute(vertex, mesh._layout, VkmVertexSemantic::Normal, scratch, 3);

                readElement(uvs, i, scratch, 2);
                vkmWriteVertexAttribute(vertex, mesh._layout, VkmVertexSemantic::UV0, scratch, 2);

                // Tangents stay zeroed when absent: nothing consumes them yet, and deriving
                // them properly needs a MikkTSpace-style generator.
                readElement(tangents, i, scratch, 4);
                vkmWriteVertexAttribute(vertex, mesh._layout, VkmVertexSemantic::Tangent, scratch, 4);
            }

            if (primitive.indices != nullptr)
            {
                mesh._indices.resize(primitive.indices->count);
                cgltf_accessor_unpack_indices(primitive.indices, mesh._indices.data(),
                                              sizeof(uint32_t), mesh._indices.size());

                for (const uint32_t index : mesh._indices)
                {
                    if (index >= mesh._vertexCount)
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

        /*
        * @brief The model-image index a texture view points at, or INVALID_VALUE32.
        *
        * @details A glTF texture is an (image, sampler) pair; only the image is taken here, because
        * set 0 carries one fixed linear/clamp sampler and there is nowhere to put a per-texture one
        * yet. Materials whose sampler differs will therefore address wrong at the edges -- noted
        * rather than silently accepted.
        */
        uint32_t imageIndexOf(const cgltf_data* data, const cgltf_texture_view& view)
        {
            if (view.texture == nullptr || view.texture->image == nullptr)
            {
                return INVALID_VALUE32;
            }
            return static_cast<uint32_t>(cgltf_image_index(data, view.texture->image));
        }

        void convertMaterial(const cgltf_data* data, const cgltf_material& source, VkmSceneMaterial* outMaterial)
        {
            outMaterial->_name = source.name != nullptr ? source.name : "";
            outMaterial->_emissiveFactor = glm::make_vec3(source.emissive_factor);
            // KHR_materials_emissive_strength: the factor itself is clamped to [0,1] by the core
            // spec, so this extension is the only way an asset expresses a bright emitter.
            if (source.has_emissive_strength)
            {
                outMaterial->_emissiveFactor *= source.emissive_strength.emissive_strength;
            }
            // Only MASK carries a cutoff. BLEND is left at 0 (drawn opaque) rather than guessed
            // at: sorted blending does not exist here, and silently masking a blended surface
            // would trade one wrong image for another.
            outMaterial->_alphaCutoff =
                (source.alpha_mode == cgltf_alpha_mode_mask) ? source.alpha_cutoff : 0.0f;
            outMaterial->_normalImage = imageIndexOf(data, source.normal_texture);
            outMaterial->_emissiveImage = imageIndexOf(data, source.emissive_texture);

            if (source.has_pbr_metallic_roughness)
            {
                const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
                outMaterial->_baseColorFactor = glm::make_vec4(pbr.base_color_factor);
                outMaterial->_metallicFactor = pbr.metallic_factor;
                outMaterial->_roughnessFactor = pbr.roughness_factor;
                outMaterial->_baseColorImage = imageIndexOf(data, pbr.base_color_texture);
                outMaterial->_metallicRoughnessImage = imageIndexOf(data, pbr.metallic_roughness_texture);
            }
        }

        /*
        * @brief Resolves each glTF image to a path next to the glTF itself.
        *
        * @details URIs in a glTF are relative to the document, so they only mean anything alongside
        * its directory. Percent-encoding is decoded in place by cgltf_decode_uri, which is why the
        * URI is copied first -- the parsed data is shared and decoding is destructive.
        *
        * An image with no URI (embedded in a buffer view, or a data URI) is left empty: neither is
        * decoded here, and an empty path is how the uploader tells there is nothing to open.
        */
        void convertImages(const cgltf_data* data, const std::string& gltfPath, VkmSceneModel* outModel)
        {
            const std::filesystem::path baseDirectory = std::filesystem::path(gltfPath).parent_path();

            outModel->_images.resize(data->images_count);
            for (cgltf_size i = 0; i < data->images_count; ++i)
            {
                const cgltf_image& image = data->images[i];
                if (image.uri == nullptr || image.uri[0] == '\0')
                {
                    continue;
                }
                std::string uri = image.uri;
                if (uri.rfind("data:", 0) == 0)
                {
                    continue; // embedded, not a file
                }
                cgltf_decode_uri(uri.data());
                uri.resize(std::char_traits<char>::length(uri.c_str()));
                outModel->_images[i]._uri = (baseDirectory / uri).string();
            }
        }
    } // namespace

    bool importGltfModel(const std::string& filePath,
                         VkmSceneModel* outModel,
                         std::string* outError,
                         const VkmGltfImportOptions& options)
    {
        VKM_PROFILE_SCOPE("Gltf::import");
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

        convertImages(data, filePath, &model);

        model._materials.resize(data->materials_count);
        for (cgltf_size i = 0; i < data->materials_count; ++i)
        {
            convertMaterial(data, data->materials[i], &model._materials[i]);
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

        // KHR_lights_punctual. cgltf parses the extension unconditionally, so an absent
        // extension simply leaves lights_count at zero.
        model._lights.reserve(data->lights_count);
        for (cgltf_size i = 0; i < data->lights_count; ++i)
        {
            const cgltf_light& source = data->lights[i];
            VkmScenePunctualLight light;
            light._name = source.name != nullptr ? source.name : "";
            light._color = glm::vec3(source.color[0], source.color[1], source.color[2]);
            light._intensity = source.intensity;
            light._range = source.range;
            light._innerConeAngle = source.spot_inner_cone_angle;
            light._outerConeAngle = source.spot_outer_cone_angle;
            switch (source.type)
            {
                case cgltf_light_type_directional: light._type = VkmLightType::Directional; break;
                case cgltf_light_type_spot:        light._type = VkmLightType::Spot; break;
                // A light whose type the file did not name is a point light rather than a
                // dropped one: dropping it would shift every later index a node refers to.
                default:                           light._type = VkmLightType::Point; break;
            }
            model._lights.push_back(std::move(light));
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

            if (sourceNode.light != nullptr)
            {
                node._lightIndex = static_cast<uint32_t>(cgltf_light_index(data, sourceNode.light));
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
