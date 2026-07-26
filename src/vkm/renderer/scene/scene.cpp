// Copyright (c) 2025 Snowapril

#include <vkm/renderer/scene/scene.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/staging_buffer.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/geometric.hpp>

#include <algorithm>

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
            // Dedicated allocation, so a bindless registration always sees offset 0.
            bufferInfo._placementHint = VkmMemoryPlacementHint::ForceCommitted;
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
        for (const VkmSceneMaterial& material : model._materials)
        {
            VkmMaterialData data;
            data._baseColorFactor = material._baseColorFactor;
            data._emissive = glm::vec4(material._emissiveFactor, 0.0f);
            data._metallicRoughness = glm::vec4(material._metallicFactor, material._roughnessFactor, 0.0f, 0.0f);
            _materials.push_back(data);
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

            // A batch breaks only on what is actually pipeline state.
            if (!_drawBatches.empty() &&
                _drawBatches.back()._pipelineId == pipelineId &&
                _drawBatches.back()._layout == entry._layout)
            {
                _drawBatches.back()._objectCount++;
                continue;
            }

            DrawBatch batch;
            batch._layout = entry._layout;
            batch._pipelineId = pipelineId;
            batch._firstObject = i;
            batch._objectCount = 1;
            _drawBatches.push_back(batch);
        }
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

            const glm::vec3 center = entry._bounds._valid ? entry._bounds.getCenter() : glm::vec3(0.0f);
            const float radius = entry._bounds._valid ? glm::length(entry._bounds.getExtent()) * 0.5f : 0.0f;
            data._boundsCenterRadius = glm::vec4(center, radius);
        }

        _dirtyFirst = 0;
        _dirtyEnd = static_cast<uint32_t>(_objectData.size());
    }

    void VkmScene::assignBatchRegions(uint64_t* outVisibleListSize, uint64_t* outArgumentSize)
    {
        // The count words come first, one per batch, so both buffers share a batch's count index.
        // Everything after them is 16-byte aligned so an argument record never straddles a
        // native alignment boundary.
        const uint32_t countWords = static_cast<uint32_t>(_drawBatches.size());
        const uint32_t regionBase = (countWords + 3u) & ~3u;

        uint32_t visibleCursor = regionBase;
        uint32_t argumentCursor = regionBase;
        for (uint32_t i = 0; i < static_cast<uint32_t>(_drawBatches.size()); ++i)
        {
            DrawBatch& batch = _drawBatches[i];
            batch._countWordOffset = i;

            batch._visibleWordOffset = visibleCursor;
            visibleCursor += batch._objectCount; // one u32 object index per candidate

            batch._argumentWordOffset = argumentCursor;
            // Same stride the draw call will use, so the region math and the fetch cannot disagree.
            argumentCursor += batch._objectCount * (vkmGetIndirectArgumentStride(kSceneArgumentLayout) / sizeof(uint32_t));
        }

        _countRegionSize = static_cast<uint64_t>(countWords) * sizeof(uint32_t);
        *outVisibleListSize = static_cast<uint64_t>(visibleCursor) * sizeof(uint32_t);
        *outArgumentSize = static_cast<uint64_t>(argumentCursor) * sizeof(uint32_t);
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

        VkmBuffer* frameDataBuffer = createStorageBuffer(driver, sizeof(VkmFrameData), "SceneFrameData");
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
        argumentInfo._placementHint = VkmMemoryPlacementHint::ForceCommitted;
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

        // One staging buffer per frame slot, laid out as [ObjectData array][FrameData].
        _frameDataStagingOffset = objectDataSize;
        for (uint32_t frame = 0; frame < FRAME_BUFFER_COUNT; ++frame)
        {
            VkmStagingBufferInfo stagingInfo{};
            stagingInfo._flags = VkmResourceCreateInfo::AllowTransferSrc;
            stagingInfo._size = objectDataSize + sizeof(VkmFrameData);
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
        return true;
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
            // Unbind before the buffers go away so the set never names a released resource.
            bindlessManager->setSingletonBuffer(VkmBindlessSingletonBuffer::ObjectData, VKM_INVALID_RESOURCE_HANDLE);
            bindlessManager->setSingletonBuffer(VkmBindlessSingletonBuffer::FrameData, VKM_INVALID_RESOURCE_HANDLE);
            bindlessManager->setSingletonBuffer(VkmBindlessSingletonBuffer::VisibleList, VKM_INVALID_RESOURCE_HANDLE);
            bindlessManager->setSingletonBuffer(VkmBindlessSingletonBuffer::IndirectArgument, VKM_INVALID_RESOURCE_HANDLE);
        }
        _materialPoolSlot = INVALID_VALUE32;
        _cullPipeline = nullptr;
        _emitPipeline = nullptr;

        VkmDeferredResourceReclaimer* reclaimer = driver->getDeferredReclaimer();
        const auto release = [reclaimer](VkmResourceHandle& handle) {
            if (reclaimer != nullptr && handle != VKM_INVALID_RESOURCE_HANDLE)
            {
                reclaimer->requestRelease(handle);
            }
            handle = VKM_INVALID_RESOURCE_HANDLE;
        };

        release(_materialBuffer);
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
        _countRegionSize = 0;
        _dirtyFirst = 0;
        _dirtyEnd = 0;
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

    void VkmScene::recordUpdate(VkmCommandBufferBase* commandBuffer, uint32_t frameIndex, const VkmFrameData& frameData)
    {
        VKM_ASSERT(commandBuffer != nullptr, "VkmScene::recordUpdate requires a command buffer");
        VKM_ASSERT(frameIndex < FRAME_BUFFER_COUNT, "VkmScene::recordUpdate frame index is out of range");

        if (_objectData.empty())
        {
            return;
        }

        VkmStagingBuffer* staging = _stagingPointers[frameIndex];
        if (staging == nullptr)
        {
            return;
        }

        // FrameData changes every frame (the camera moves), so it is always copied.
        VkmFrameData publishedFrameData = frameData;
        publishedFrameData._materialPoolSlot = _materialPoolSlot;
        staging->writeDirect(_frameDataStagingOffset, &publishedFrameData, sizeof(VkmFrameData));
        commandBuffer->copyBuffer(_stagingBuffers[frameIndex], _frameDataBuffer,
                                  _frameDataStagingOffset, 0, sizeof(VkmFrameData));

        if (_dirtyFirst != _dirtyEnd)
        {
            const uint64_t byteOffset = static_cast<uint64_t>(_dirtyFirst) * sizeof(VkmObjectData);
            const uint64_t byteSize = static_cast<uint64_t>(_dirtyEnd - _dirtyFirst) * sizeof(VkmObjectData);
            staging->writeDirect(byteOffset, _objectData.data() + _dirtyFirst, byteSize);
            commandBuffer->copyBuffer(_stagingBuffers[frameIndex], _objectDataBuffer, byteOffset, byteOffset, byteSize);
            _dirtyFirst = 0;
            _dirtyEnd = 0;
        }
    }

    void VkmScene::recordCull(VkmCommandBufferBase* commandBuffer)
    {
        VKM_ASSERT(commandBuffer != nullptr, "VkmScene::recordCull requires a command buffer");

        if (_drawBatches.empty() || _cullPipeline == nullptr || _emitPipeline == nullptr)
        {
            return;
        }

        // Every batch's visible count has to start at zero: the cull pass only ever increments it.
        commandBuffer->copyBuffer(_countClearBuffer, _visibleListBuffer, 0, 0, _countRegionSize);
        commandBuffer->barrierIndirectArgumentBuffer(_visibleListBuffer);

        const auto dispatchBatches = [this, commandBuffer](VkmPipelineStateBase* pipeline) {
            commandBuffer->bindPipeline(pipeline);
            for (const DrawBatch& batch : _drawBatches)
            {
                SceneBatchConstants constants{};
                constants._firstObject = batch._firstObject;
                constants._objectCount = batch._objectCount;
                constants._visibleWordOffset = batch._visibleWordOffset;
                constants._countWordOffset = batch._countWordOffset;
                constants._argumentWordOffset = batch._argumentWordOffset;
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
        commandBuffer->barrierIndirectArgumentBuffer(_visibleListBuffer);

        dispatchBatches(_emitPipeline);
        commandBuffer->barrierIndirectArgumentBuffer(_argumentBuffer);
    }

    void VkmScene::recordDrawBatches(VkmCommandBufferBase* commandBuffer,
                                     const std::function<VkmPipelineStateBase*(const DrawBatch&)>& pipelineResolver)
    {
        VKM_ASSERT(commandBuffer != nullptr, "VkmScene::recordDrawBatches requires a command buffer");

        for (const DrawBatch& batch : _drawBatches)
        {
            VkmPipelineStateBase* pipeline = pipelineResolver(batch);
            if (pipeline == nullptr)
            {
                VKM_DEBUG_WARN("VkmScene: skipping a draw batch with no pipeline for its vertex layout");
                continue;
            }
            commandBuffer->bindPipeline(pipeline);

            // Nothing is pushed per draw: the arguments the emit pass wrote carry each survivor's
            // object index in firstInstance, which the vertex shader reads as SV_InstanceID.
            commandBuffer->drawIndirectCount(
                kSceneArgumentLayout,
                _argumentBuffer, static_cast<uint64_t>(batch._argumentWordOffset) * sizeof(uint32_t),
                _argumentBuffer, static_cast<uint64_t>(batch._countWordOffset) * sizeof(uint32_t),
                batch._objectCount);
        }
    }

    void VkmScene::collectReferencedResources(std::vector<VkmResourceHandle>* outHandles) const
    {
        VKM_ASSERT(outHandles != nullptr, "VkmScene::collectReferencedResources requires an output vector");

        const auto append = [outHandles](VkmResourceHandle handle) {
            if (handle != VKM_INVALID_RESOURCE_HANDLE)
            {
                outHandles->push_back(handle);
            }
        };

        for (const std::unique_ptr<VkmSceneGeometryPool>& pool : _pools)
        {
            if (pool != nullptr)
            {
                append(pool->getVertexBuffer());
                append(pool->getIndexBuffer());
            }
        }
        append(_materialBuffer);
        append(_objectDataBuffer);
        append(_frameDataBuffer);
        append(_visibleListBuffer);
        append(_argumentBuffer);
        append(_countClearBuffer);
        for (VkmResourceHandle staging : _stagingBuffers)
        {
            append(staging);
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
