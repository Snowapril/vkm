// Copyright (c) 2025 Snowapril

#include <vkm/renderer/scene/scene_model_gpu.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>

namespace vkm
{
    namespace
    {
        // Matches the triangle sample's flags: AllowShaderWrite is what maps to a storage
        // buffer (AllowShaderRead would make it a uniform buffer, which cannot back a
        // bindless storage-buffer array element), and AllowTransferSrc is required by the
        // WebGPU backend, whose registerBuffer() copies into its bindless mega-buffer.
        constexpr VkmResourceCreateInfo kSceneBufferFlags =
            static_cast<VkmResourceCreateInfo>(
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderWrite) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferSrc));

        VkmBuffer* createAndUploadBuffer(VkmDriverBase* driver, const void* data, uint64_t size, const std::string& debugName)
        {
            VkmBufferInfo bufferInfo{};
            bufferInfo._flags = kSceneBufferFlags;
            bufferInfo._size = size;
            // Dedicated allocation, so the bindless registration always sees offset 0.
            bufferInfo._placementHint = VkmMemoryPlacementHint::ForceCommitted;
            bufferInfo._debugName = debugName.c_str();

            VkmBuffer* buffer = driver->newBuffer(bufferInfo);
            if (buffer == nullptr)
            {
                return nullptr;
            }

            if (!driver->uploadToBuffer(buffer->getHandle(), data, size))
            {
                driver->getRenderResourcePool()->releaseResource(buffer->getHandle());
                return nullptr;
            }
            return buffer;
        }
    } // namespace

    bool VkmSceneModelGpu::upload(VkmDriverBase* driver, const VkmSceneModel& model, std::string* outError)
    {
        VKM_ASSERT(driver != nullptr, "VkmSceneModelGpu::upload requires a driver");

        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        if (bindlessManager == nullptr)
        {
            if (outError != nullptr)
            {
                *outError = "The driver has no bindless resource manager";
            }
            return false;
        }

        // Entries stay index-aligned with model._meshes so VkmSceneModel::DrawItem's mesh
        // index addresses both sides; meshes with no geometry get an empty entry.
        _meshes.resize(model._meshes.size());

        for (size_t meshIndex = 0; meshIndex < model._meshes.size(); ++meshIndex)
        {
            const VkmSceneMesh& mesh = model._meshes[meshIndex];
            if (mesh._vertices.empty() || mesh._indices.empty())
            {
                continue;
            }

            const std::string namePrefix = "SceneMesh" + std::to_string(meshIndex);

            VkmBuffer* vertexBuffer = createAndUploadBuffer(
                driver, mesh._vertices.data(), mesh._vertices.size() * sizeof(VkmSceneVertex), namePrefix + "VertexBuffer");
            if (vertexBuffer == nullptr)
            {
                destroy(driver);
                if (outError != nullptr)
                {
                    *outError = "Failed to upload the vertex buffer of mesh " + std::to_string(meshIndex);
                }
                return false;
            }

            VkmBuffer* indexBuffer = createAndUploadBuffer(
                driver, mesh._indices.data(), mesh._indices.size() * sizeof(uint32_t), namePrefix + "IndexBuffer");
            if (indexBuffer == nullptr)
            {
                driver->getRenderResourcePool()->releaseResource(vertexBuffer->getHandle());
                destroy(driver);
                if (outError != nullptr)
                {
                    *outError = "Failed to upload the index buffer of mesh " + std::to_string(meshIndex);
                }
                return false;
            }

            MeshGpu& meshGpu = _meshes[meshIndex];
            meshGpu._vertexBuffer = vertexBuffer->getHandle();
            meshGpu._indexBuffer = indexBuffer->getHandle();
            meshGpu._indexCount = static_cast<uint32_t>(mesh._indices.size());
            meshGpu._vertexBufferSlot = bindlessManager->registerBuffer(meshGpu._vertexBuffer, VkmBindlessArrayType::Buffer);
            meshGpu._indexBufferSlot = bindlessManager->registerBuffer(meshGpu._indexBuffer, VkmBindlessArrayType::IndexBuffer);

            if (meshGpu._vertexBufferSlot == INVALID_VALUE32 || meshGpu._indexBufferSlot == INVALID_VALUE32)
            {
                destroy(driver);
                if (outError != nullptr)
                {
                    *outError = "The bindless buffer arrays are exhausted at mesh " + std::to_string(meshIndex);
                }
                return false;
            }
        }

        return true;
    }

    void VkmSceneModelGpu::destroy(VkmDriverBase* driver)
    {
        VKM_ASSERT(driver != nullptr, "VkmSceneModelGpu::destroy requires a driver");

        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();

        for (MeshGpu& mesh : _meshes)
        {
            if (bindlessManager != nullptr)
            {
                if (mesh._vertexBufferSlot != INVALID_VALUE32)
                {
                    bindlessManager->unregisterBuffer(mesh._vertexBufferSlot, VkmBindlessArrayType::Buffer);
                }
                if (mesh._indexBufferSlot != INVALID_VALUE32)
                {
                    bindlessManager->unregisterBuffer(mesh._indexBufferSlot, VkmBindlessArrayType::IndexBuffer);
                }
            }

            if (resourcePool != nullptr)
            {
                if (mesh._vertexBuffer != VKM_INVALID_RESOURCE_HANDLE)
                {
                    resourcePool->releaseResource(mesh._vertexBuffer);
                }
                if (mesh._indexBuffer != VKM_INVALID_RESOURCE_HANDLE)
                {
                    resourcePool->releaseResource(mesh._indexBuffer);
                }
            }
        }

        _meshes.clear();
    }
} // namespace vkm
