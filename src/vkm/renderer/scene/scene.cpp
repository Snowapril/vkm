// Copyright (c) 2026 Snowapril

#include <vkm/renderer/scene/scene.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/acceleration_structure.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/buffer_view.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/scene/image_loader.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cstring>
#include <map>

namespace vkm
{
    namespace
    {
        // See VkmSceneGeometryPool's kPoolBufferFlags for why these three flags are the recipe for
        // a bindless-registerable storage buffer.
        constexpr VkmResourceCreateInfo kSceneStorageBufferFlags =
            static_cast<VkmResourceCreateInfo>(
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderWrite) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferSrc));

        /*
        * The scene's emit pass writes non-indexed records: index fetching happens in the vertex
        * shader out of a bindless pool, so a draw's vertexCount is its index count. Named once here
        * because both the argument-region sizing and the draw call have to agree on it.
        */
        constexpr VkmIndirectArgumentLayout kSceneArgumentLayout = VkmIndirectArgumentLayout::NonIndexed;

        bool fail(std::string* outError, const std::string& message)
        {
            if (outError != nullptr)
            {
                *outError = message;
            }
            return false;
        }

        VkmBuffer* createStorageBuffer(VkmDriverBase* driver, uint64_t size, const char* debugName)
        {
            VkmBufferInfo bufferInfo{};
            bufferInfo._flags = kSceneStorageBufferFlags;
            bufferInfo._size = size;
            bufferInfo._debugName = debugName;
            return driver->newBuffer(bufferInfo);
        }
    } // namespace

    void vkmExtractFrustumPlanes(const glm::mat4& viewProjection, glm::vec4* outPlanes)
    {
        VKM_ASSERT(outPlanes != nullptr, "vkmExtractFrustumPlanes requires an output array of 6 planes");

        // Gribb/Hartmann: each clip-space plane inequality, pulled back through the combined
        // matrix, is a row combination of it. glm is column-major, so row i is
        // (m[0][i], m[1][i], m[2][i], m[3][i]).
        const auto row = [&viewProjection](int i) {
            return glm::vec4(viewProjection[0][i], viewProjection[1][i], viewProjection[2][i], viewProjection[3][i]);
        };

        const glm::vec4 rowX = row(0);
        const glm::vec4 rowY = row(1);
        const glm::vec4 rowZ = row(2);
        const glm::vec4 rowW = row(3);

        // The engine's clip space is [0,1] in depth on every backend, so near is +Z (not W + Z).
        const glm::vec4 planes[6] = {
            rowW + rowX, // left
            rowW - rowX, // right
            rowW + rowY, // bottom
            rowW - rowY, // top
            rowZ,        // near
            rowW - rowZ, // far
        };

        for (int i = 0; i < 6; ++i)
        {
            const float length = glm::length(glm::vec3(planes[i]));
            // A degenerate matrix (e.g. a zero-extent viewport) would divide by zero; leaving such
            // a plane at zero makes its half-space test trivially pass, which is the conservative
            // outcome for a culling test.
            outPlanes[i] = length > 0.0f ? planes[i] / length : glm::vec4(0.0f);
        }
    }

    VkmScene::VkmScene()
    {
        _stagingBuffers.fill(VKM_INVALID_RESOURCE_HANDLE);
        _stagingPointers.fill(nullptr);
    }

    bool VkmScene::addModel(const VkmSceneModel& model, std::string* outError)
    {
        VKM_ASSERT(_objectData.empty(), "VkmScene::addModel must be called before build()");

        const uint32_t materialBase = static_cast<uint32_t>(_materials.size());
        // An image index the model does not resolve to a file URI leaves an empty path, which
        // uploadMaterialTextures() reads as "no texture" -- the same outcome as no reference at
        // all. The importer already leaves _uri empty for buffer-view and data-URI images.
        const auto imagePath = [&model](uint32_t imageIndex) -> std::string {
            return (imageIndex < model._images.size()) ? model._images[imageIndex]._uri : std::string();
        };
        for (const VkmSceneMaterial& material : model._materials)
        {
            VkmMaterialData data;
            data._baseColorFactor = material._baseColorFactor;
            // w carries the alpha-mask cutoff: word 7 of the GPU record was unused, so a
            // masked material costs no extra bytes and no layout change.
            data._emissive = glm::vec4(material._emissiveFactor, material._alphaCutoff);
            data._metallicRoughness = glm::vec4(material._metallicFactor, material._roughnessFactor, 0.0f, 0.0f);
            _materials.push_back(data);

            // Paths, not pixels: addModel() has no driver, and a glTF's images are usually larger
            // than its geometry, so decoding waits for build().
            MaterialImageRefs refs;
            refs._baseColor = imagePath(material._baseColorImage);
            refs._metallicRoughness = imagePath(material._metallicRoughnessImage);
            refs._normal = imagePath(material._normalImage);
            refs._emissive = imagePath(material._emissiveImage);
            _materialImages.push_back(std::move(refs));
        }

        // Mesh entry index per model mesh, so the draw list below can map onto the pooled ranges.
        // A mesh with no geometry gets INVALID_VALUE32 and its draw items are dropped.
        std::vector<uint32_t> meshEntryIndices(model._meshes.size(), INVALID_VALUE32);

        for (size_t meshIndex = 0; meshIndex < model._meshes.size(); ++meshIndex)
        {
            const VkmSceneMesh& mesh = model._meshes[meshIndex];
            if (mesh._vertexCount == 0 || mesh._indices.empty())
            {
                continue;
            }

            const size_t poolIndex = static_cast<size_t>(mesh._layout._preset);
            if (_pools[poolIndex] == nullptr)
            {
                _pools[poolIndex] = std::make_unique<VkmSceneGeometryPool>(mesh._layout._preset);
            }

            VkmSceneGeometryPool::MeshRange range;
            if (!_pools[poolIndex]->appendMesh(mesh, &range, outError))
            {
                return false;
            }

            MeshEntry entry;
            entry._layout = mesh._layout._preset;
            entry._range = range;
            entry._materialIndex = mesh._materialIndex == INVALID_VALUE32
                                       ? INVALID_VALUE32
                                       : materialBase + mesh._materialIndex;
            entry._bounds = mesh._bounds;

            // Gathered here, while the model still owns its CPU vertex bytes -- the pool clears
            // its own copy at upload, and build() only sees ranges. Object space; build() expands
            // per placed object once the world transforms are final.
            if (entry._materialIndex != INVALID_VALUE32)
            {
                const glm::vec4& emissive = _materials[entry._materialIndex]._emissive;
                if (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f)
                {
                    vkmGatherEmissiveTriangles(mesh, glm::vec3(emissive), &entry._emissiveTriangles);
                }
            }

            meshEntryIndices[meshIndex] = static_cast<uint32_t>(_meshEntries.size());
            _meshEntries.push_back(entry);
        }

        for (const VkmSceneModel::DrawItem& item : model.buildDrawList())
        {
            if (item._meshIndex >= meshEntryIndices.size())
            {
                continue;
            }
            const uint32_t meshEntryIndex = meshEntryIndices[item._meshIndex];
            if (meshEntryIndex == INVALID_VALUE32)
            {
                continue;
            }

            VkmSceneObject object;
            object._worldTransform = item._worldTransform;
            object._meshEntryIndex = meshEntryIndex;
            _objects.push_back(object);
        }

        // Batching depends only on the objects and their mesh entries, so it is kept up to date
        // here rather than in build(): that keeps getDrawBatches() meaningful (and testable)
        // without a driver, and build() only has to fill the GPU records afterwards.
        buildDrawBatches();
        return true;
    }

    void VkmScene::buildDrawBatches()
    {
        // Objects are ordered by (pipelineId, layout, materialIndex) so that a batch is a
        // contiguous run and materials of a batch land near each other in the material pool.
        std::stable_sort(_objects.begin(), _objects.end(), [this](const VkmSceneObject& lhs, const VkmSceneObject& rhs) {
            if (lhs._pipelineId != rhs._pipelineId)
            {
                return lhs._pipelineId < rhs._pipelineId;
            }
            const MeshEntry& lhsEntry = _meshEntries[lhs._meshEntryIndex];
            const MeshEntry& rhsEntry = _meshEntries[rhs._meshEntryIndex];
            if (lhsEntry._layout != rhsEntry._layout)
            {
                return lhsEntry._layout < rhsEntry._layout;
            }
            return lhsEntry._materialIndex < rhsEntry._materialIndex;
        });

        _drawBatches.clear();
        for (uint32_t i = 0; i < static_cast<uint32_t>(_objects.size()); ++i)
        {
            const MeshEntry& entry = _meshEntries[_objects[i]._meshEntryIndex];
            const uint32_t pipelineId = _objects[i]._pipelineId;

            // A batch breaks on pipeline state *and* on material. Material is not pipeline state,
            // but a backend without bindless textures binds a per-material set-3 table before the
            // draw, and a table is per-draw -- so one material per batch is what makes that
            // expressible at all. It costs one more drawIndirectCount per material run; the total
            // encoded draws are unchanged on Metal and WebGPU, which already encode maxDrawCount
            // per batch. Split on every backend rather than only where it is needed, so there is
            // one code path and it can be exercised where the result can be looked at.
            if (!_drawBatches.empty() &&
                _drawBatches.back()._pipelineId == pipelineId &&
                _drawBatches.back()._layout == entry._layout &&
                _drawBatches.back()._materialIndex == entry._materialIndex)
            {
                _drawBatches.back()._objectCount++;
                continue;
            }

            DrawBatch batch;
            batch._layout = entry._layout;
            batch._pipelineId = pipelineId;
            batch._materialIndex = entry._materialIndex;
            batch._firstObject = i;
            batch._objectCount = 1;
            _drawBatches.push_back(batch);
        }
    }

    void VkmScene::setDirectionalRadiance(const glm::vec3& radiance)
    {
        VKM_ASSERT(_lightBuffer == VKM_INVALID_RESOURCE_HANDLE,
                   "setDirectionalRadiance must precede build(): the light table uploads once");
        _directionalRadiance = radiance;
    }

    void VkmScene::fillObjectData()
    {
        _objectData.resize(_objects.size());
        for (size_t i = 0; i < _objects.size(); ++i)
        {
            const VkmSceneObject& object = _objects[i];
            const MeshEntry& entry = _meshEntries[object._meshEntryIndex];
            const VkmSceneGeometryPool& pool = *_pools[static_cast<size_t>(entry._layout)];

            VkmObjectData& data = _objectData[i];
            data._worldTransform = object._worldTransform;
            data._normalTransform = glm::mat4(glm::inverseTranspose(glm::mat3(object._worldTransform)));
            data._vertexPoolSlot = pool.getVertexPoolSlot();
            data._indexPoolSlot = pool.getIndexPoolSlot();
            data._vertexWordOffset = entry._range._vertexWordOffset;
            // A primitive with no material shades with material 0's factors rather than reading
            // out of range; an empty material pool cannot happen because build() always uploads
            // at least one record.
            data._materialIndex = entry._materialIndex == INVALID_VALUE32 ? 0u : entry._materialIndex;
            data._indexOffset = entry._range._indexOffset;
            data._indexCount = entry._range._indexCount;
            // Every preset's stride is a whole number of words (16, 64 and 32 bytes), which is
            // what lets the pools be untyped word arrays in the first place.
            data._vertexStrideWords = pool.getLayout()._stride / 4;

            const glm::vec3 center = entry._bounds._valid ? entry._bounds.getCenter() : glm::vec3(0.0f);
            const float radius = entry._bounds._valid ? glm::length(entry._bounds.getExtent()) * 0.5f : 0.0f;
            data._boundsCenterRadius = glm::vec4(center, radius);
        }

        _dirtyFirst = 0;
        _dirtyEnd = static_cast<uint32_t>(_objectData.size());
    }

    void VkmScene::assignBatchRegions(uint64_t* outVisibleListSize, uint64_t* outArgumentSize)
    {
        // The count words come first -- every view's, back to back -- so both buffers share a
        // batch's count index even though their payloads have different strides. Everything after
        // them is 16-byte aligned so an argument record never straddles a native alignment
        // boundary. The offsets stored per batch are view 0's; recordCull() and recordDrawBatches()
        // add the view's stride.
        const uint32_t batchCount = static_cast<uint32_t>(_drawBatches.size());
        const uint32_t countWords = batchCount * kVkmSceneMaxCullViews;
        const uint32_t regionBase = (countWords + 3u) & ~3u;

        uint32_t visibleCursor = regionBase;
        uint32_t argumentCursor = regionBase;
        for (uint32_t i = 0; i < batchCount; ++i)
        {
            DrawBatch& batch = _drawBatches[i];
            batch._countWordOffset = i;

            batch._visibleWordOffset = visibleCursor;
            visibleCursor += batch._objectCount; // one u32 object index per candidate

            batch._argumentWordOffset = argumentCursor;
            // Same stride the draw call will use, so the region math and the fetch cannot disagree.
            argumentCursor += batch._objectCount * (vkmGetIndirectArgumentStride(kSceneArgumentLayout) / sizeof(uint32_t));
        }

        _visibleViewStrideWords = visibleCursor - regionBase;
        _argumentViewStrideWords = argumentCursor - regionBase;
        // One view's worth: a cull clears only the counts it is about to fill.
        _countRegionSize = static_cast<uint64_t>(batchCount) * sizeof(uint32_t);
        *outVisibleListSize =
            static_cast<uint64_t>(regionBase + _visibleViewStrideWords * kVkmSceneMaxCullViews) * sizeof(uint32_t);
        *outArgumentSize =
            static_cast<uint64_t>(regionBase + _argumentViewStrideWords * kVkmSceneMaxCullViews) * sizeof(uint32_t);
    }

    /*
    * @brief Decodes, uploads and registers every image the scene's materials reference.
    *
    * @details Runs from build(), which is the first point a driver exists. One texture per
    * (path, colour space): glTF multiplies factor by texture and the two colour spaces are
    * different data, so an image referenced as both base colour and metallic-roughness genuinely
    * needs two.
    *
    * A single image failing to decode warns and leaves that slot invalid, which falls back to the
    * material's factor, rather than failing the whole scene -- one unreadable PNG should not cost
    * the geometry. Returning false is reserved for running out of bindless slots, where continuing
    * would silently mis-address whatever slot came back.
    */
    VkmScene::MaterialTextures VkmScene::getMaterialTextures(uint32_t materialIndex) const
    {
        return (materialIndex < _materialTextureHandles.size()) ? _materialTextureHandles[materialIndex]
                                                                : MaterialTextures{};
    }

    bool VkmScene::uploadMaterialTextures(VkmDriverBase* driver, std::string* outError)
    {
        if ((driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::TextureUpload) == 0u)
        {
            return true;
        }

        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        if (bindlessManager == nullptr)
        {
            return true;
        }

        // Two separate capabilities. WebGPU can upload pixels but has no set-0 texture array to
        // index them from, so there the textures are created and kept -- getMaterialTexture()
        // hands them to a per-material set-3 table -- while every slot stays INVALID_VALUE32 and
        // the shader's bindless branch is never taken.
        const bool bindlessAvailable =
            (driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::BindlessTextures) != 0u;

        // Streaming re-points a bindless slot at a rebuilt texture, so it is exactly the bindless
        // backends it can run on. Elsewhere a material's textures reach the shader through an
        // immutable per-draw table, which has no slot to re-point and cannot be patched in place.
        _textureStreamingAvailable = bindlessAvailable;

        // Keyed on (path, sRGB) so one image serving two colour spaces gets one texture each.
        std::map<std::pair<std::string, bool>, uint32_t> uploaded;
        std::map<std::pair<std::string, bool>, VkmResourceHandle> textureHandles;
        // Same key again, naming the streamer entry that owns the texture, so the per-material
        // loop below can record which channels sample it.
        std::map<std::pair<std::string, bool>, uint32_t> streamerEntries;
        bool slotsExhausted = false;

        const auto slotFor = [&](const std::string& path, bool srgb) -> uint32_t {
            if (path.empty() || slotsExhausted)
            {
                return INVALID_VALUE32;
            }
            const auto key = std::make_pair(path, srgb);
            const auto existing = uploaded.find(key);
            if (existing != uploaded.end())
            {
                return existing->second;
            }

            VkmImageData image;
            std::string imageError;
            if (!loadImageFromFile(path, &image, &imageError))
            {
                VKM_DEBUG_WARN(("Material texture '" + path + "' could not be decoded (" + imageError +
                                "); the material falls back to its factor").c_str());
                uploaded.emplace(key, INVALID_VALUE32);
                return INVALID_VALUE32;
            }

            VkmTextureInfo info{};
            info._flags = static_cast<VkmResourceCreateInfo>(
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst));
            // A full mip chain. Without one, a minified material texture samples a single texel
            // per pixel and sparkles under any camera motion -- Sponza's roof tiles are the
            // obvious case. The levels are built on the CPU and uploaded like any other level,
            // which needs no new GPU path: uploadToTexture has always taken a mip index.
            std::vector<VkmImageData> mipLevels;
            vkmBuildMipChain(image, srgb, &mipLevels);

            info._extent = glm::uvec3(image._width, image._height, 1);
            info._numMipLevels = 1u + static_cast<uint32_t>(mipLevels.size());
            info._numArrayLayers = 1;
            // The colour space belongs to the channel that references the image, not to the file:
            // base colour and emissive are sRGB-encoded, metallic-roughness and normal are linear
            // data that must not be de-gamma'd on the way in.
            info._format = srgb ? VkmFormat::R8G8B8A8_SRGB : VkmFormat::R8G8B8A8_UNORM;
            info._debugName = "SceneMaterialTexture";

            VkmTexture* texture = driver->newTexture(info);
            bool uploadedAllLevels = texture != nullptr;
            if (uploadedAllLevels)
            {
                uploadedAllLevels =
                    driver->uploadToTexture(texture->getHandle(), image._pixels.data(), image.getByteSize());
                for (size_t level = 0; uploadedAllLevels && level < mipLevels.size(); ++level)
                {
                    uploadedAllLevels = driver->uploadToTexture(
                        texture->getHandle(), mipLevels[level]._pixels.data(), mipLevels[level].getByteSize(),
                        static_cast<uint32_t>(level + 1));
                }
            }
            if (!uploadedAllLevels)
            {
                VKM_DEBUG_WARN(("Material texture '" + path +
                                "' could not be uploaded; the material falls back to its factor").c_str());
                if (texture != nullptr)
                {
                    driver->getRenderResourcePool()->releaseResource(texture->getHandle());
                }
                uploaded.emplace(key, INVALID_VALUE32);
                return INVALID_VALUE32;
            }

            uint32_t slot = INVALID_VALUE32;
            if (bindlessAvailable)
            {
                slot = bindlessManager->registerTexture(texture->getHandle());
                if (slot == INVALID_VALUE32)
                {
                    driver->getRenderResourcePool()->releaseResource(texture->getHandle());
                    slotsExhausted = true;
                    return INVALID_VALUE32;
                }
                // The streamer takes ownership of the texture and its slot from here: a rebuild
                // replaces both, so a second record of either would name a released resource.
                // Level 0 is what was just uploaded, and the first streaming ticks take it down
                // from there -- build() has no camera to size it against.
                const uint32_t entryIndex = _textureStreamer.addTexture(
                    path, srgb, glm::uvec2(image._width, image._height), info._numMipLevels,
                    /*residentBaseMip=*/0u, texture->getHandle(), slot);
                streamerEntries.emplace(key, entryIndex);
            }
            else
            {
                _materialTextures.push_back(texture->getHandle());
            }
            textureHandles.emplace(key, texture->getHandle());
            uploaded.emplace(key, slot);
            return slot;
        };

        // Per-material handles regardless of bindless: a set-3 table binds the texture itself
        // rather than a slot index, so this is what the WebGPU path consumes.
        _materialTextureHandles.assign(_materials.size(), {});
        const auto handleFor = [&](const std::string& path, bool srgb) -> VkmResourceHandle {
            const auto existing = textureHandles.find(std::make_pair(path, srgb));
            return (existing != textureHandles.end()) ? existing->second : VKM_INVALID_RESOURCE_HANDLE;
        };

        // Tells the streamer that material `materialIndex`'s `channel` samples this image, so a
        // rebuild knows every record it has to re-point.
        const auto referenceFor = [&](const std::string& path, bool srgb, uint32_t materialIndex,
                                      uint32_t channel) {
            const auto existing = streamerEntries.find(std::make_pair(path, srgb));
            if (existing != streamerEntries.end())
            {
                _textureStreamer.addReference(existing->second, materialIndex, channel);
            }
        };

        for (size_t i = 0; i < _materials.size(); ++i)
        {
            const MaterialImageRefs& refs = _materialImages[i];
            glm::uvec4& slots = _materials[i]._textureSlots;
            slots.x = slotFor(refs._baseColor, /*srgb=*/true);
            slots.y = slotFor(refs._metallicRoughness, /*srgb=*/false);
            slots.z = slotFor(refs._normal, /*srgb=*/false);
            slots.w = slotFor(refs._emissive, /*srgb=*/true);

            const uint32_t materialIndex = static_cast<uint32_t>(i);
            referenceFor(refs._baseColor, /*srgb=*/true, materialIndex, 0);
            referenceFor(refs._metallicRoughness, /*srgb=*/false, materialIndex, 1);
            referenceFor(refs._normal, /*srgb=*/false, materialIndex, 2);
            referenceFor(refs._emissive, /*srgb=*/true, materialIndex, 3);

            MaterialTextures& handles = _materialTextureHandles[i];
            handles._baseColor = handleFor(refs._baseColor, true);
            handles._metallicRoughness = handleFor(refs._metallicRoughness, false);
            handles._emissive = handleFor(refs._emissive, true);
        }

        // The debug view reads the base-colour channel's level out of the record, so it has to
        // start describing what was actually uploaded rather than zero.
        for (size_t i = 0; i < _materials.size(); ++i)
        {
            const uint32_t materialIndex = static_cast<uint32_t>(i);
            const uint32_t entryIndex = _textureStreamer.findEntry(materialIndex, /*channel=*/0);
            if (entryIndex != INVALID_VALUE32)
            {
                publishStreamingMip(materialIndex, /*channel=*/0, _textureStreamer.getResidentBaseMip(entryIndex),
                                    _textureStreamer.getTotalMipCount(entryIndex));
            }
        }
        // Nothing has reached the GPU yet -- build() uploads the whole array right after this.
        _materialDirtyFirst = 0;
        _materialDirtyEnd = 0;

        if (slotsExhausted)
        {
            return fail(outError, "The bindless texture array is exhausted while uploading material textures");
        }
        return true;
    }

    bool VkmScene::build(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager, std::string* outError)
    {
        VKM_ASSERT(driver != nullptr, "VkmScene::build requires a driver");
        VKM_ASSERT(pipelineStateManager != nullptr, "VkmScene::build requires a pipeline state manager");

        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        if (bindlessManager == nullptr)
        {
            return fail(outError, "The driver has no bindless resource manager");
        }

        if (_objects.empty())
        {
            return fail(outError, "The scene has no drawable object");
        }

        for (std::unique_ptr<VkmSceneGeometryPool>& pool : _pools)
        {
            if (pool != nullptr && !pool->upload(driver, outError))
            {
                destroy(driver);
                return false;
            }
        }

        // At least one record, so a primitive without a material can index 0 safely.
        if (_materials.empty())
        {
            _materials.push_back(VkmMaterialData{});
        }
        _materialImages.resize(_materials.size());

        // Fills _materials' texture slots, so this must precede the pool upload below.
        if (!uploadMaterialTextures(driver, outError))
        {
            destroy(driver);
            return false;
        }

        VkmBuffer* materialBuffer = createStorageBuffer(
            driver, _materials.size() * sizeof(VkmMaterialData), "SceneMaterialPool");
        if (materialBuffer == nullptr ||
            !driver->uploadToBuffer(materialBuffer->getHandle(), _materials.data(), _materials.size() * sizeof(VkmMaterialData)))
        {
            if (materialBuffer != nullptr)
            {
                _materialBuffer = materialBuffer->getHandle();
            }
            destroy(driver);
            return fail(outError, "Failed to upload the scene's material pool");
        }
        _materialBuffer = materialBuffer->getHandle();
        _materialPoolSlot = bindlessManager->registerBuffer(_materialBuffer, VkmBindlessArrayType::Buffer);
        if (_materialPoolSlot == INVALID_VALUE32)
        {
            destroy(driver);
            return fail(outError, "The bindless buffer array is exhausted while registering the material pool");
        }

        // The object array's slots must be known before the records are filled, so the buffers are
        // created first and populated by the initial recordUpdate().
        const uint64_t objectDataSize = _objects.size() * sizeof(VkmObjectData);
        VkmBuffer* objectDataBuffer = createStorageBuffer(driver, objectDataSize, "SceneObjectData");
        if (objectDataBuffer == nullptr)
        {
            destroy(driver);
            return fail(outError, "Failed to create the scene's object-data buffer");
        }
        _objectDataBuffer = objectDataBuffer->getHandle();

        VkmBuffer* frameDataBuffer =
            createStorageBuffer(driver, sizeof(VkmFrameData) * kVkmSceneMaxCullViews, "SceneFrameData");
        if (frameDataBuffer == nullptr)
        {
            destroy(driver);
            return fail(outError, "Failed to create the scene's frame-data buffer");
        }
        _frameDataBuffer = frameDataBuffer->getHandle();

        if (!bindlessManager->setSingletonBuffer(VkmBindlessSingletonBuffer::ObjectData, _objectDataBuffer) ||
            !bindlessManager->setSingletonBuffer(VkmBindlessSingletonBuffer::FrameData, _frameDataBuffer))
        {
            destroy(driver);
            return fail(outError, "Failed to publish the scene's per-object / per-frame buffers into the bindless set");
        }

        _cullPipeline = pipelineStateManager->getPipelineState("scene_cull_pso[default]", VkmPipelineStateOrigin::Engine);
        _emitPipeline = pipelineStateManager->getPipelineState("scene_emit_draws_pso[default]", VkmPipelineStateOrigin::Engine);
        if (_cullPipeline == nullptr || _emitPipeline == nullptr)
        {
            destroy(driver);
            return fail(outError, "The engine's scene culling / emit compute pipelines are not loaded");
        }

        uint64_t visibleListSize = 0;
        uint64_t argumentSize = 0;
        assignBatchRegions(&visibleListSize, &argumentSize);

        VkmBuffer* visibleListBuffer = createStorageBuffer(driver, visibleListSize, "SceneVisibleList");
        if (visibleListBuffer == nullptr)
        {
            destroy(driver);
            return fail(outError, "Failed to create the scene's visible-list buffer");
        }
        _visibleListBuffer = visibleListBuffer->getHandle();

        // The only buffer the draw side fetches arguments from, so it also needs AllowIndirectBuffer.
        VkmBufferInfo argumentInfo{};
        argumentInfo._flags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(kSceneStorageBufferFlags) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowIndirectBuffer));
        argumentInfo._size = argumentSize;
        argumentInfo._debugName = "SceneIndirectArguments";
        VkmBuffer* argumentBuffer = driver->newBuffer(argumentInfo);
        if (argumentBuffer == nullptr)
        {
            destroy(driver);
            return fail(outError, "Failed to create the scene's indirect argument buffer");
        }
        _argumentBuffer = argumentBuffer->getHandle();

        if (!bindlessManager->setSingletonBuffer(VkmBindlessSingletonBuffer::VisibleList, _visibleListBuffer) ||
            !bindlessManager->setSingletonBuffer(VkmBindlessSingletonBuffer::IndirectArgument, _argumentBuffer))
        {
            destroy(driver);
            return fail(outError, "Failed to publish the scene's GPU-driven buffers into the bindless set");
        }

        // A persistent zero-filled source for the per-frame count reset. Uploaded once; the reset
        // itself is one small copyBuffer per frame rather than a dispatch.
        {
            const std::vector<uint8_t> zeros(static_cast<size_t>(_countRegionSize), 0);
            VkmBuffer* clearBuffer = createStorageBuffer(driver, _countRegionSize, "SceneCountClear");
            if (clearBuffer == nullptr ||
                !driver->uploadToBuffer(clearBuffer->getHandle(), zeros.data(), _countRegionSize))
            {
                if (clearBuffer != nullptr)
                {
                    _countClearBuffer = clearBuffer->getHandle();
                }
                destroy(driver);
                return fail(outError, "Failed to create the scene's count-clear buffer");
            }
            _countClearBuffer = clearBuffer->getHandle();
        }

        // One staging buffer per frame slot, laid out as
        // [ObjectData array][FrameData per view][MaterialData array]. The material region is the
        // streaming path's: a rebuilt texture lands on a new bindless slot, so its records have to
        // reach the GPU again, and they take the same per-frame-slot route the object data does.
        const uint64_t materialDataSize = _materials.size() * sizeof(VkmMaterialData);
        _frameDataStagingOffset = objectDataSize;
        _materialStagingOffset = _frameDataStagingOffset + sizeof(VkmFrameData) * kVkmSceneMaxCullViews;
        for (uint32_t frame = 0; frame < FRAME_BUFFER_COUNT; ++frame)
        {
            VkmStagingBufferInfo stagingInfo{};
            stagingInfo._flags = VkmResourceCreateInfo::AllowTransferSrc;
            stagingInfo._size = _materialStagingOffset + materialDataSize;
            stagingInfo._debugName = "SceneUploadStaging";

            VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
            if (staging == nullptr)
            {
                destroy(driver);
                return fail(outError, "Failed to create the scene's upload staging buffers");
            }
            _stagingBuffers[frame] = staging->getHandle();
            _stagingPointers[frame] = staging;
        }

        fillObjectData();

        if (!buildLightTable(driver, bindlessManager, outError))
        {
            destroy(driver);
            return false;
        }

        // Last, so a failed build never leaves a worker running against a half-torn-down scene.
        if (_textureStreamingAvailable)
        {
            _textureStreamer.start();
        }
        return true;
    }

    bool VkmScene::buildLightTable(VkmDriverBase* driver, VkmBindlessResourceManagerBase* bindlessManager,
                                   std::string* outError)
    {
        // Expand each placed object's object-space emissive triangles by its final world
        // transform. Baked at build: a moved emitter needs a rebuild (TODO.md).
        std::vector<VkmLightTableTriangle> triangles;
        for (const VkmSceneObject& object : _objects)
        {
            const MeshEntry& entry = _meshEntries[object._meshEntryIndex];
            for (const VkmLightTableTriangle& local : entry._emissiveTriangles)
            {
                VkmLightTableTriangle world = local;
                float* corners[3] = { world._p0, world._p1, world._p2 };
                for (float* corner : corners)
                {
                    const glm::vec4 transformed =
                        object._worldTransform * glm::vec4(corner[0], corner[1], corner[2], 1.0f);
                    corner[0] = transformed.x;
                    corner[1] = transformed.y;
                    corner[2] = transformed.z;
                }
                triangles.push_back(world);
            }
        }
        _lightTriangleCount = static_cast<uint32_t>(vkmFinalizeLightTable(&triangles));

        /*
        * Always created, even header-only: an unconditionally valid _lightPoolSlot means no
        * INVALID branch exists in any shader, and a light-less scene is a data-driven no-op
        * (zero sun, zero count).
        */
        VkmLightTableHeader header{};
        header._sunRadiance[0] = _directionalRadiance.x;
        header._sunRadiance[1] = _directionalRadiance.y;
        header._sunRadiance[2] = _directionalRadiance.z;

        const uint64_t byteSize =
            sizeof(VkmLightTableHeader) + triangles.size() * sizeof(VkmLightTableTriangle);
        std::vector<uint8_t> bytes(byteSize);
        std::memcpy(bytes.data(), &header, sizeof(header));
        if (!triangles.empty())
        {
            std::memcpy(bytes.data() + sizeof(header), triangles.data(),
                        triangles.size() * sizeof(VkmLightTableTriangle));
        }

        VkmBuffer* lightBuffer = createStorageBuffer(driver, byteSize, "SceneLightTable");
        if (lightBuffer == nullptr ||
            !driver->uploadToBuffer(lightBuffer->getHandle(), bytes.data(), byteSize))
        {
            if (lightBuffer != nullptr)
            {
                _lightBuffer = lightBuffer->getHandle();
            }
            return fail(outError, "Failed to upload the scene's light table");
        }
        _lightBuffer = lightBuffer->getHandle();
        _lightPoolSlot = bindlessManager->registerBuffer(_lightBuffer, VkmBindlessArrayType::Buffer);
        if (_lightPoolSlot == INVALID_VALUE32)
        {
            return fail(outError, "The bindless buffer array is exhausted while registering the light table");
        }
        return true;
    }

    namespace
    {
        // A format-less view over a range of a geometry pool buffer. Format-less on purpose: a
        // build reads raw vertex and index memory, so no VkBufferView is created for it and the
        // view is metadata the backends resolve to an address.
        VkmResourceHandle makeGeometryView(VkmDriverBase* driver, VkmResourceHandle buffer,
                                           uint64_t offset, uint64_t size, const char* debugName)
        {
            if (buffer == VKM_INVALID_RESOURCE_HANDLE || size == 0)
            {
                return VKM_INVALID_RESOURCE_HANDLE;
            }
            VkmBuffer* parent = driver->getRenderResourcePool()->getResource<VkmBuffer>(buffer);
            if (parent == nullptr)
            {
                return VKM_INVALID_RESOURCE_HANDLE;
            }
            VkmBufferViewInfo viewInfo{};
            viewInfo._offset = offset;
            viewInfo._size = size;
            viewInfo._debugName = debugName;
            // createView rather than newBufferView: it records the view as a child of the buffer,
            // so releasing the geometry pool releases these with it and the scene tracks nothing.
            VkmBufferView* view = parent->createView(viewInfo);
            return view != nullptr ? view->getHandle() : VKM_INVALID_RESOURCE_HANDLE;
        }
    } // namespace

    bool VkmScene::buildAccelerationStructures(VkmDriverBase* driver, std::string* outError)
    {
        VKM_ASSERT(driver != nullptr, "VkmScene::buildAccelerationStructures requires a driver");

        if ((driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::RayTracing) == 0)
        {
            return fail(outError, "This device reports no ray tracing capability");
        }
        if (_objectData.empty())
        {
            return fail(outError, "VkmScene::buildAccelerationStructures must follow a successful build()");
        }
        if (!_meshStructures.empty())
        {
            return fail(outError, "The scene's acceleration structures were already built");
        }

        _meshStructures.assign(_meshEntries.size(), VKM_INVALID_RESOURCE_HANDLE);
        for (size_t entry = 0; entry < _meshEntries.size(); ++entry)
        {
            const MeshEntry& meshEntry = _meshEntries[entry];
            const VkmSceneGeometryPool* pool = _pools[static_cast<size_t>(meshEntry._layout)].get();
            VKM_ASSERT(pool != nullptr, "A mesh entry names a vertex layout whose pool was never created");

            const std::string debugName = "SceneBlas[" + std::to_string(entry) + "]";

            // The pool addresses vertices in u32 words and indices in u32 elements; both are four
            // bytes, which is also the alignment a float3 vertex format and a u32 index type need.
            const std::string vertexViewName = debugName + ".Vertices";
            const std::string indexViewName = debugName + ".Indices";
            const VkmResourceHandle vertexView = makeGeometryView(
                driver, pool->getVertexBuffer(),
                static_cast<uint64_t>(meshEntry._range._vertexWordOffset) * sizeof(uint32_t),
                static_cast<uint64_t>(meshEntry._range._vertexCount) * pool->getLayout()._stride,
                vertexViewName.c_str());
            const VkmResourceHandle indexView = makeGeometryView(
                driver, pool->getIndexBuffer(),
                static_cast<uint64_t>(meshEntry._range._indexOffset) * sizeof(uint32_t),
                static_cast<uint64_t>(meshEntry._range._indexCount) * sizeof(uint32_t),
                indexViewName.c_str());
            if (vertexView == VKM_INVALID_RESOURCE_HANDLE || indexView == VKM_INVALID_RESOURCE_HANDLE)
            {
                releaseAccelerationStructures(driver);
                return fail(outError, "Failed to view the geometry pool for " + debugName);
            }
            VkmAccelerationStructureGeometry geometry{};
            geometry._vertexView = vertexView;
            // Position is attribute 0 of every VkmVertexLayoutPreset, so the pool's stride is the
            // whole description a triangle geometry needs -- the build reads nothing else.
            geometry._vertexStride = pool->getLayout()._stride;
            geometry._vertexCount = meshEntry._range._vertexCount;
            geometry._indexView = indexView;
            geometry._indexCount = meshEntry._range._indexCount;

            VkmAccelerationStructureInfo blasInfo{};
            blasInfo._type = VkmAccelerationStructureType::BottomLevel;
            blasInfo._debugName = debugName.c_str();
            blasInfo._geometries.push_back(geometry);
            if (meshEntry._bounds._valid)
            {
                blasInfo._boundsMin = meshEntry._bounds._min;
                blasInfo._boundsMax = meshEntry._bounds._max;
            }

            VkmAccelerationStructure* blas = driver->newAccelerationStructure(blasInfo);
            if (blas == nullptr)
            {
                releaseAccelerationStructures(driver);
                return fail(outError, "Failed to build " + debugName);
            }
            _meshStructures[entry] = blas->getHandle();
        }

        VkmAccelerationStructureInfo tlasInfo{};
        tlasInfo._type = VkmAccelerationStructureType::TopLevel;
        tlasInfo._debugName = "SceneTlas";
        // Rebuildable, because a scene's objects move. Note the limit this inherits: the structure
        // is sized against this instance list, so recordAccelerationStructureUpdate() can move the
        // objects but a scene that *spawns* one has to build its structures again (TODO.md).
        tlasInfo._allowUpdate = true;
        collectInstances(&tlasInfo._instances);

        VkmAccelerationStructure* tlas = driver->newAccelerationStructure(tlasInfo);
        if (tlas == nullptr)
        {
            releaseAccelerationStructures(driver);
            return fail(outError, "Failed to build the scene's top-level acceleration structure");
        }
        _topLevelStructure = tlas->getHandle();
        // Kept for the same reason the staging buffers are: VkmCommandBufferBase exposes no driver,
        // so the per-frame update cannot look the structure up by handle.
        _topLevelStructurePointer = tlas;

        // Published once, not per frame: a rebuild writes into the same structure, so the
        // descriptor stays valid across every recordAccelerationStructureUpdate().
        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        if (bindlessManager == nullptr || !bindlessManager->setAccelerationStructure(_topLevelStructure))
        {
            releaseAccelerationStructures(driver);
            return fail(outError, "Failed to publish the scene's acceleration structure into the bindless set");
        }
        return true;
    }

    void VkmScene::collectInstances(std::vector<VkmAccelerationStructureInstance>* outInstances) const
    {
        outInstances->clear();
        outInstances->reserve(_objects.size());
        for (size_t object = 0; object < _objects.size(); ++object)
        {
            const uint32_t entry = _objects[object]._meshEntryIndex;
            if (entry >= _meshStructures.size() || _meshStructures[entry] == VKM_INVALID_RESOURCE_HANDLE)
            {
                continue;
            }
            VkmAccelerationStructureInstance instance{};
            instance._transform = _objects[object]._worldTransform;
            instance._blas = _meshStructures[entry];
            // The object index, which is also the VkmObjectData index -- _objects is the sorted
            // array the records were filled from, so a hit recovers its object through this alone.
            instance._instanceId = static_cast<uint32_t>(object);
            outInstances->push_back(instance);
        }
    }

    void VkmScene::recordAccelerationStructureUpdate(VkmCommandBufferBase* commandBuffer)
    {
        VKM_ASSERT(commandBuffer != nullptr, "VkmScene::recordAccelerationStructureUpdate requires a command buffer");

        if (_topLevelStructurePointer == nullptr)
        {
            VKM_DEBUG_ERROR("recordAccelerationStructureUpdate before buildAccelerationStructures()");
            return;
        }

        std::vector<VkmAccelerationStructureInstance> instances;
        collectInstances(&instances);
        // A host write into the buffer the recorded build reads, not a recorded command; see the
        // hazard recorded in TODO.md.
        if (!_topLevelStructurePointer->updateInstances(instances))
        {
            return;
        }
        commandBuffer->buildAccelerationStructure(_topLevelStructure);
    }

    void VkmScene::releaseAccelerationStructures(VkmDriverBase* driver)
    {
        // Unbind before the structure goes away, so the set never names a released resource --
        // the same ordering destroy() keeps for the singleton buffers.
        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        if (bindlessManager != nullptr && _topLevelStructure != VKM_INVALID_RESOURCE_HANDLE)
        {
            bindlessManager->setAccelerationStructure(VKM_INVALID_RESOURCE_HANDLE);
        }

        VkmDeferredResourceReclaimer* reclaimer = driver->getDeferredReclaimer();
        const auto release = [reclaimer](VkmResourceHandle& handle) {
            if (reclaimer != nullptr && handle != VKM_INVALID_RESOURCE_HANDLE)
            {
                reclaimer->requestRelease(handle);
            }
            handle = VKM_INVALID_RESOURCE_HANDLE;
        };

        // The top-level structure first: it names the bottom-level ones, so releasing it last would
        // leave the reclaimer free to drop a structure something still instances.
        release(_topLevelStructure);
        _topLevelStructurePointer = nullptr;
        for (VkmResourceHandle& structure : _meshStructures)
        {
            release(structure);
        }
        _meshStructures.clear();
    }

    void VkmScene::destroy(VkmDriverBase* driver)
    {
        VKM_ASSERT(driver != nullptr, "VkmScene::destroy requires a driver");

        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        if (bindlessManager != nullptr)
        {
            if (_materialPoolSlot != INVALID_VALUE32)
            {
                bindlessManager->unregisterBuffer(_materialPoolSlot, VkmBindlessArrayType::Buffer);
            }
            if (_lightPoolSlot != INVALID_VALUE32)
            {
                bindlessManager->unregisterBuffer(_lightPoolSlot, VkmBindlessArrayType::Buffer);
            }
            // Unbind before the buffers go away so the set never names a released resource.
            bindlessManager->setSingletonBuffer(VkmBindlessSingletonBuffer::ObjectData, VKM_INVALID_RESOURCE_HANDLE);
            bindlessManager->setSingletonBuffer(VkmBindlessSingletonBuffer::FrameData, VKM_INVALID_RESOURCE_HANDLE);
            bindlessManager->setSingletonBuffer(VkmBindlessSingletonBuffer::VisibleList, VKM_INVALID_RESOURCE_HANDLE);
            bindlessManager->setSingletonBuffer(VkmBindlessSingletonBuffer::IndirectArgument, VKM_INVALID_RESOURCE_HANDLE);
        }
        _materialPoolSlot = INVALID_VALUE32;
        _lightPoolSlot = INVALID_VALUE32;
        _lightTriangleCount = 0;
        _cullPipeline = nullptr;
        _emitPipeline = nullptr;

        releaseAccelerationStructures(driver);

        VkmDeferredResourceReclaimer* reclaimer = driver->getDeferredReclaimer();
        const auto release = [reclaimer](VkmResourceHandle& handle) {
            if (reclaimer != nullptr && handle != VKM_INVALID_RESOURCE_HANDLE)
            {
                reclaimer->requestRelease(handle);
            }
            handle = VKM_INVALID_RESOURCE_HANDLE;
        };

        // Owns the streamed textures and their slots on a bindless backend, so it has to run
        // before the buffers below go away.
        _textureStreamer.destroy(driver);
        _textureStreamingAvailable = false;

        for (VkmResourceHandle& texture : _materialTextures)
        {
            release(texture);
        }
        _materialTextures.clear();
        // Both are filled in lockstep with _materials -- _materialImages by addModel(), the handles
        // by uploadMaterialTextures() -- so leaving either behind makes the next addModel() append
        // onto a stale prefix that build()'s resize() then keeps in place of the new scene's.
        _materialImages.clear();
        _materialTextureHandles.clear();

        release(_materialBuffer);
        release(_lightBuffer);
        release(_objectDataBuffer);
        release(_frameDataBuffer);
        release(_visibleListBuffer);
        release(_argumentBuffer);
        release(_countClearBuffer);
        for (VkmResourceHandle& staging : _stagingBuffers)
        {
            release(staging);
        }
        _stagingPointers.fill(nullptr);

        for (std::unique_ptr<VkmSceneGeometryPool>& pool : _pools)
        {
            if (pool != nullptr)
            {
                pool->destroy(driver);
                pool.reset();
            }
        }

        _meshEntries.clear();
        _objects.clear();
        _materials.clear();
        _objectData.clear();
        _drawBatches.clear();
        _frameDataStagingOffset = 0;
        _materialStagingOffset = 0;
        _countRegionSize = 0;
        _dirtyFirst = 0;
        _dirtyEnd = 0;
        _materialDirtyFirst = 0;
        _materialDirtyEnd = 0;
        _streamingObjects.clear();
        _streamingUpdates.clear();
    }

    void VkmScene::setObjectTransform(uint32_t objectIndex, const glm::mat4& worldTransform)
    {
        VKM_ASSERT(objectIndex < _objectData.size(), "VkmScene::setObjectTransform index is out of range");

        _objects[objectIndex]._worldTransform = worldTransform;
        _objectData[objectIndex]._worldTransform = worldTransform;
        _objectData[objectIndex]._normalTransform = glm::mat4(glm::inverseTranspose(glm::mat3(worldTransform)));

        if (_dirtyFirst == _dirtyEnd)
        {
            _dirtyFirst = objectIndex;
            _dirtyEnd = objectIndex + 1;
            return;
        }
        _dirtyFirst = std::min(_dirtyFirst, objectIndex);
        _dirtyEnd = std::max(_dirtyEnd, objectIndex + 1);
    }

    void VkmScene::recordUpdate(VkmCommandBufferBase* commandBuffer, uint32_t frameIndex,
                                const VkmFrameData& frameData, uint32_t viewIndex)
    {
        VKM_ASSERT(commandBuffer != nullptr, "VkmScene::recordUpdate requires a command buffer");
        VKM_ASSERT(frameIndex < FRAME_BUFFER_COUNT, "VkmScene::recordUpdate frame index is out of range");
        VKM_ASSERT(viewIndex < kVkmSceneMaxCullViews, "VkmScene::recordUpdate view index is out of range");

        if (_objectData.empty())
        {
            return;
        }

        VkmStagingBuffer* staging = _stagingPointers[frameIndex];
        if (staging == nullptr)
        {
            return;
        }

        // FrameData changes every frame (the camera moves), so it is always copied. Each view owns
        // a staging region and a device region: the host write below happens now, while the copy
        // happens whenever the GPU reaches it, so two views sharing either would race.
        const uint64_t viewByteOffset = static_cast<uint64_t>(viewIndex) * sizeof(VkmFrameData);
        VkmFrameData publishedFrameData = frameData;
        publishedFrameData._materialPoolSlot = _materialPoolSlot;
        publishedFrameData._lightPoolSlot = _lightPoolSlot;
        publishedFrameData._lightCount = _lightTriangleCount;
        staging->writeDirect(_frameDataStagingOffset + viewByteOffset, &publishedFrameData, sizeof(VkmFrameData));
        commandBuffer->copyBuffer(_stagingBuffers[frameIndex], _frameDataBuffer,
                                  _frameDataStagingOffset + viewByteOffset, viewByteOffset, sizeof(VkmFrameData));

        if (_dirtyFirst != _dirtyEnd)
        {
            const uint64_t byteOffset = static_cast<uint64_t>(_dirtyFirst) * sizeof(VkmObjectData);
            const uint64_t byteSize = static_cast<uint64_t>(_dirtyEnd - _dirtyFirst) * sizeof(VkmObjectData);
            staging->writeDirect(byteOffset, _objectData.data() + _dirtyFirst, byteSize);
            commandBuffer->copyBuffer(_stagingBuffers[frameIndex], _objectDataBuffer, byteOffset, byteOffset, byteSize);
            _dirtyFirst = 0;
            _dirtyEnd = 0;
        }

        if (_materialDirtyFirst != _materialDirtyEnd)
        {
            const uint64_t byteOffset = static_cast<uint64_t>(_materialDirtyFirst) * sizeof(VkmMaterialData);
            const uint64_t byteSize =
                static_cast<uint64_t>(_materialDirtyEnd - _materialDirtyFirst) * sizeof(VkmMaterialData);
            staging->writeDirect(_materialStagingOffset + byteOffset, _materials.data() + _materialDirtyFirst,
                                 byteSize);
            commandBuffer->copyBuffer(_stagingBuffers[frameIndex], _materialBuffer,
                                      _materialStagingOffset + byteOffset, byteOffset, byteSize);
            _materialDirtyFirst = 0;
            _materialDirtyEnd = 0;
        }
    }

    void VkmScene::markMaterialDirty(uint32_t materialIndex)
    {
        VKM_ASSERT(materialIndex < _materials.size(), "VkmScene::markMaterialDirty index is out of range");

        if (_materialDirtyFirst == _materialDirtyEnd)
        {
            _materialDirtyFirst = materialIndex;
            _materialDirtyEnd = materialIndex + 1;
            return;
        }
        _materialDirtyFirst = std::min(_materialDirtyFirst, materialIndex);
        _materialDirtyEnd = std::max(_materialDirtyEnd, materialIndex + 1);
    }

    void VkmScene::publishStreamingMip(uint32_t materialIndex, uint32_t channel, uint32_t baseMip,
                                       uint32_t totalMipCount)
    {
        // Only the base-colour channel has somewhere to go: the record carries one pair of words,
        // and a material's four textures stream independently. The other channels still stream,
        // they are just not what the debug view shows.
        if (channel != 0u || materialIndex >= _materials.size())
        {
            return;
        }
        _materials[materialIndex]._metallicRoughness.z = static_cast<float>(baseMip);
        _materials[materialIndex]._metallicRoughness.w = static_cast<float>(totalMipCount);
    }

    uint32_t VkmScene::getStreamedBaseMip(uint32_t materialIndex, uint32_t channel) const
    {
        const uint32_t entryIndex = _textureStreamer.findEntry(materialIndex, channel);
        return (entryIndex == INVALID_VALUE32) ? INVALID_VALUE32 : _textureStreamer.getResidentBaseMip(entryIndex);
    }

    void VkmScene::updateTextureStreaming(VkmDriverBase* driver, const VkmTextureStreamingView& view)
    {
        VKM_ASSERT(driver != nullptr, "VkmScene::updateTextureStreaming requires a driver");

        if (!_textureStreamingAvailable || _objectData.empty())
        {
            return;
        }

        // The streamer measures world-space spheres, so the object-space bounds every record
        // carries are transformed here -- the radius by the largest scale the transform applies,
        // which is the conservative choice for a sphere under a non-uniform one.
        _streamingObjects.clear();
        _streamingObjects.reserve(_objectData.size());
        for (const VkmObjectData& object : _objectData)
        {
            const glm::mat3 linear(object._worldTransform);
            const float maxScale = std::sqrt(std::max({ glm::dot(linear[0], linear[0]),
                                                        glm::dot(linear[1], linear[1]),
                                                        glm::dot(linear[2], linear[2]) }));

            VkmTextureStreamingObject entry;
            entry._worldCenter =
                glm::vec3(object._worldTransform * glm::vec4(glm::vec3(object._boundsCenterRadius), 1.0f));
            entry._worldRadius = object._boundsCenterRadius.w * maxScale;
            entry._materialIndex = object._materialIndex;
            _streamingObjects.push_back(entry);
        }

        _streamingUpdates.clear();
        _textureStreamer.update(driver, view, _streamingObjects, &_streamingUpdates);

        for (const VkmTextureStreamingUpdate& update : _streamingUpdates)
        {
            if (update._materialIndex >= _materials.size() || update._channel > 3u)
            {
                continue;
            }
            _materials[update._materialIndex]._textureSlots[static_cast<int>(update._channel)] =
                update._bindlessSlot;
            publishStreamingMip(update._materialIndex, update._channel, update._baseMip, update._totalMipCount);
            markMaterialDirty(update._materialIndex);
        }
    }

    void VkmScene::recordCull(VkmCommandBufferBase* commandBuffer, uint32_t viewIndex)
    {
        VKM_ASSERT(commandBuffer != nullptr, "VkmScene::recordCull requires a command buffer");
        VKM_ASSERT(viewIndex < kVkmSceneMaxCullViews, "VkmScene::recordCull view index is out of range");

        if (_drawBatches.empty() || _cullPipeline == nullptr || _emitPipeline == nullptr)
        {
            return;
        }

        const uint32_t batchCount = static_cast<uint32_t>(_drawBatches.size());
        const uint32_t countWordBase = viewIndex * batchCount;
        const uint32_t visibleWordBase = viewIndex * _visibleViewStrideWords;
        const uint32_t argumentWordBase = viewIndex * _argumentViewStrideWords;

        // Every batch's visible count has to start at zero: the cull pass only ever increments it.
        // Only this view's counts, so a cull recorded earlier in the frame keeps its results.
        commandBuffer->copyBuffer(_countClearBuffer, _visibleListBuffer, 0,
                                  static_cast<uint64_t>(countWordBase) * sizeof(uint32_t), _countRegionSize);

        // These three hazards are *inside* one subgraph, so the render graph's analysis cannot see
        // them -- it works from what a subgraph declares, and a callback is opaque. They stay
        // explicit, but each now names the access pair it actually orders instead of sharing one
        // coarse barrier that was reused for all three.
        const VkmResourceBarrier clearToCull{
            ._handle = _visibleListBuffer,
            ._srcAccess = VkmResourceAccess::TransferWrite,
            ._dstAccess = VkmResourceAccess::ShaderStorageReadWrite,
            ._srcScope = VkmPipelineScope::Transfer,
            ._dstScope = VkmPipelineScope::Compute,
        };
        commandBuffer->resourceBarrier(&clearToCull, 1);

        const auto dispatchBatches = [&](VkmPipelineStateBase* pipeline) {
            commandBuffer->bindPipeline(pipeline);
            for (const DrawBatch& batch : _drawBatches)
            {
                SceneBatchConstants constants{};
                constants._firstObject = batch._firstObject;
                constants._objectCount = batch._objectCount;
                constants._visibleWordOffset = batch._visibleWordOffset + visibleWordBase;
                constants._countWordOffset = batch._countWordOffset + countWordBase;
                constants._argumentWordOffset = batch._argumentWordOffset + argumentWordBase;
                constants._frameDataIndex = viewIndex;
                commandBuffer->setPushConstants(&constants, sizeof(constants));

                const uint32_t groupCount =
                    (batch._objectCount + kVkmComputeThreadGroupSizeX - 1) / kVkmComputeThreadGroupSizeX;
                commandBuffer->dispatch(groupCount);
            }
            // Closing the compute pass is what orders the next stage after this one: the backends
            // publish a pass's writes at its boundary (Metal emits its queue-scope barrier here,
            // WebGPU's pass boundary is implicit).
            commandBuffer->unbindPipeline();
        };

        // Culling is shared by every backend; only the emit pass differs (this HLSL one fills the
        // indirect arguments; a Metal ICB encoder would fill commands instead).
        dispatchBatches(_cullPipeline);
        const VkmResourceBarrier cullToEmit{
            ._handle = _visibleListBuffer,
            ._srcAccess = VkmResourceAccess::ShaderStorageReadWrite,
            ._dstAccess = VkmResourceAccess::ShaderStorageRead,
            ._srcScope = VkmPipelineScope::Compute,
            ._dstScope = VkmPipelineScope::Compute,
        };
        commandBuffer->resourceBarrier(&cullToEmit, 1);

        dispatchBatches(_emitPipeline);
        // The emit pass's writes have to reach the indirect fetch in the *draw* subgraph. That one
        // is cross-subgraph, so once the render graph emits its plan this barrier is the graph's
        // job -- the draw subgraph declares _argumentBuffer as IndirectArgument and the cull
        // subgraph declares it as a storage write, which is exactly this dependency.
        const VkmResourceBarrier emitToDraw{
            ._handle = _argumentBuffer,
            ._srcAccess = VkmResourceAccess::ShaderStorageWrite,
            ._dstAccess = VkmResourceAccess::IndirectArgument,
            ._srcScope = VkmPipelineScope::Compute,
            ._dstScope = VkmPipelineScope::Graphics,
        };
        commandBuffer->resourceBarrier(&emitToDraw, 1);
    }

    void VkmScene::recordDrawBatches(VkmCommandBufferBase* commandBuffer,
                                     const std::function<VkmPipelineStateBase*(const DrawBatch&)>& pipelineResolver,
                                     const std::function<void(VkmCommandBufferBase*, const DrawBatch&)>& beforeDraw,
                                     uint32_t viewIndex)
    {
        VKM_ASSERT(commandBuffer != nullptr, "VkmScene::recordDrawBatches requires a command buffer");
        VKM_ASSERT(viewIndex < kVkmSceneMaxCullViews, "VkmScene::recordDrawBatches view index is out of range");

        // Must match the view recordCull() filled, or the draws fetch another view's arguments.
        const uint32_t countWordBase = viewIndex * static_cast<uint32_t>(_drawBatches.size());
        const uint32_t argumentWordBase = viewIndex * _argumentViewStrideWords;

        for (const DrawBatch& batch : _drawBatches)
        {
            VkmPipelineStateBase* pipeline = pipelineResolver(batch);
            if (pipeline == nullptr)
            {
                VKM_DEBUG_WARN("VkmScene: skipping a draw batch with no pipeline for its vertex layout");
                continue;
            }
            commandBuffer->bindPipeline(pipeline);

            // The one point where per-draw state can be set: push constants need a bound pipeline,
            // and this method owns the bind.
            if (beforeDraw)
            {
                beforeDraw(commandBuffer, batch);
            }

            // Nothing else is pushed per draw: the arguments the emit pass wrote carry each
            // survivor's object index in firstInstance, which the vertex shader reads as
            // SV_InstanceID.
            commandBuffer->drawIndirectCount(
                kSceneArgumentLayout,
                _argumentBuffer,
                static_cast<uint64_t>(batch._argumentWordOffset + argumentWordBase) * sizeof(uint32_t),
                _argumentBuffer,
                static_cast<uint64_t>(batch._countWordOffset + countWordBase) * sizeof(uint32_t),
                batch._objectCount);
        }
    }

    void VkmScene::collectReferencedResources(ReferencePhase phase,
                                              std::vector<VkmResourceAccessDeclaration>* outDeclarations) const
    {
        VKM_ASSERT(outDeclarations != nullptr, "VkmScene::collectReferencedResources requires an output vector");

        const auto append = [outDeclarations](VkmResourceHandle handle, VkmResourceAccess access) {
            if (handle != VKM_INVALID_RESOURCE_HANDLE)
            {
                outDeclarations->push_back(VkmResourceAccessDeclaration{ handle, access, {} });
            }
        };

        switch (phase)
        {
            case ReferencePhase::Update:
                // recordUpdate copies one frame slot's staging buffer into the two buffers whose
                // contents change every frame. Every slot is declared rather than just the one
                // this frame uses: the caller has the frame index and this method does not, and
                // over-declaring a read costs one redundant handle, not a wrong barrier.
                for (VkmResourceHandle staging : _stagingBuffers)
                {
                    append(staging, VkmResourceAccess::TransferRead);
                }
                append(_frameDataBuffer, VkmResourceAccess::TransferWrite);
                append(_objectDataBuffer, VkmResourceAccess::TransferWrite);
                // Written only on a frame where a streamed texture changed slots, but declared
                // unconditionally for the same reason every staging buffer is: the declaration is
                // what the subgraph may touch, not what it turned out to touch this frame.
                append(_materialBuffer, VkmResourceAccess::TransferWrite);
                break;

            case ReferencePhase::Cull:
                // The count clear is a copy, and both dispatches then read-modify-write the
                // lists. The hazards *between* those three are intra-subgraph and are ordered by
                // recordCull's own barriers; what is declared here is what the subgraph as a
                // whole consumes and publishes.
                append(_countClearBuffer, VkmResourceAccess::TransferRead);
                append(_visibleListBuffer, VkmResourceAccess::TransferWrite);
                append(_visibleListBuffer, VkmResourceAccess::ShaderStorageReadWrite);
                append(_argumentBuffer, VkmResourceAccess::ShaderStorageReadWrite);
                append(_objectDataBuffer, VkmResourceAccess::ShaderStorageRead);
                append(_frameDataBuffer, VkmResourceAccess::ShaderStorageRead);
                break;

            case ReferencePhase::Draw:
                // Geometry and material data are pulled in-shader out of the bindless set; the
                // draw count and the argument records come out of the one argument buffer.
                for (const std::unique_ptr<VkmSceneGeometryPool>& pool : _pools)
                {
                    if (pool != nullptr)
                    {
                        append(pool->getVertexBuffer(), VkmResourceAccess::ShaderStorageRead);
                        append(pool->getIndexBuffer(), VkmResourceAccess::ShaderStorageRead);
                    }
                }
                append(_materialBuffer, VkmResourceAccess::ShaderStorageRead);
                append(_lightBuffer, VkmResourceAccess::ShaderStorageRead);
                append(_objectDataBuffer, VkmResourceAccess::ShaderStorageRead);
                append(_frameDataBuffer, VkmResourceAccess::ShaderStorageRead);
                append(_argumentBuffer, VkmResourceAccess::IndirectArgument);
                break;
        }
    }

    uint64_t VkmScene::getTotalIndexCount() const
    {
        uint64_t count = 0;
        for (const VkmObjectData& data : _objectData)
        {
            count += data._indexCount;
        }
        return count;
    }

    VkmSceneAABB VkmScene::computeWorldBounds() const
    {
        VkmSceneAABB bounds;
        for (const VkmSceneObject& object : _objects)
        {
            const MeshEntry& entry = _meshEntries[object._meshEntryIndex];
            if (entry._bounds._valid)
            {
                bounds.expand(entry._bounds.transformed(object._worldTransform));
            }
        }
        return bounds;
    }
} // namespace vkm
