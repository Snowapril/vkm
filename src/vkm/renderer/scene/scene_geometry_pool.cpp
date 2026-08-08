// Copyright (c) 2025 Snowapril

#include <vkm/renderer/scene/scene_geometry_pool.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>

#include <limits>

namespace vkm
{
    namespace
    {
        // AllowShaderWrite is what maps to a storage buffer (AllowShaderRead would make it a
        // uniform buffer, which cannot back a bindless storage-buffer array element), and
        // AllowTransferSrc is required by the WebGPU backend, whose registerBuffer() copies into
        // its bindless mega-buffer.
        constexpr VkmResourceCreateInfo kPoolBufferFlags =
            static_cast<VkmResourceCreateInfo>(
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderWrite) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferSrc));

        /*
        * @brief The pool's buffer flags for `driver`.
        *
        * `AllowAccelerationStructureInput` is added only where the device reports ray tracing: on
        * Vulkan it becomes `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR`,
        * which is not a legal usage on a device where `VK_KHR_acceleration_structure` was never
        * enabled. Where it is added it costs nothing: it also forces the committed allocation
        * path, which these buffers already ask for.
        */
        VkmResourceCreateInfo poolBufferFlags(VkmDriverBase* driver)
        {
            if ((driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::RayTracing) == 0)
            {
                return kPoolBufferFlags;
            }
            return static_cast<VkmResourceCreateInfo>(
                static_cast<uint32_t>(kPoolBufferFlags) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowAccelerationStructureInput));
        }

        VkmBuffer* createAndUploadBuffer(VkmDriverBase* driver, const void* data, uint64_t size, const std::string& debugName)
        {
            VkmBufferInfo bufferInfo{};
            bufferInfo._flags = poolBufferFlags(driver);
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
                driver->getDeferredReclaimer()->requestRelease(buffer->getHandle());
                return nullptr;
            }
            return buffer;
        }

        bool fail(std::string* outError, const std::string& message)
        {
            if (outError != nullptr)
            {
                *outError = message;
            }
            return false;
        }
    } // namespace

    VkmSceneGeometryPool::VkmSceneGeometryPool(VkmVertexLayoutPreset preset)
        : _layout(vkmGetVertexLayoutPreset(preset))
    {
    }

    bool VkmSceneGeometryPool::appendMesh(const VkmSceneMesh& mesh, MeshRange* outRange, std::string* outError)
    {
        VKM_ASSERT(outRange != nullptr, "VkmSceneGeometryPool::appendMesh requires an output range");
        VKM_ASSERT(mesh._layout._preset == _layout._preset,
                   "A mesh may only be appended to the pool of its own vertex layout preset");
        VKM_ASSERT(mesh._vertexData.size() == static_cast<size_t>(mesh._vertexCount) * _layout._stride,
                   "VkmSceneMesh::_vertexData size disagrees with _vertexCount * stride");

        // Both offsets are u32 in the shader-side ObjectData record, so the pool cannot grow past
        // what they can address.
        constexpr size_t kMaxOffset = std::numeric_limits<uint32_t>::max();
        if (_vertexBytes.size() + mesh._vertexData.size() > kMaxOffset * 4)
        {
            return fail(outError, "The geometry pool's vertex buffer would exceed the addressable u32 word range");
        }
        if (_indices.size() + mesh._indices.size() > kMaxOffset)
        {
            return fail(outError, "The geometry pool's index buffer would exceed the addressable u32 element range");
        }

        MeshRange range;
        range._vertexWordOffset = static_cast<uint32_t>(_vertexBytes.size() / 4);
        range._vertexCount = mesh._vertexCount;
        range._indexOffset = static_cast<uint32_t>(_indices.size());
        range._indexCount = static_cast<uint32_t>(mesh._indices.size());

        _vertexBytes.insert(_vertexBytes.end(), mesh._vertexData.begin(), mesh._vertexData.end());
        // Indices stay mesh-local: range._vertexWordOffset supplies the base in-shader.
        _indices.insert(_indices.end(), mesh._indices.begin(), mesh._indices.end());
        _indexCount = static_cast<uint32_t>(_indices.size());

        *outRange = range;
        return true;
    }

    bool VkmSceneGeometryPool::upload(VkmDriverBase* driver, std::string* outError)
    {
        VKM_ASSERT(driver != nullptr, "VkmSceneGeometryPool::upload requires a driver");

        if (_vertexBytes.empty() || _indices.empty())
        {
            return true; // Nothing to publish; the pool stays unregistered and isEmpty().
        }

        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        if (bindlessManager == nullptr)
        {
            return fail(outError, "The driver has no bindless resource manager");
        }

        const std::string namePrefix = std::string("SceneGeometryPool[") + vkmVertexLayoutPresetName(_layout._preset) + "]";

        VkmBuffer* vertexBuffer = createAndUploadBuffer(driver, _vertexBytes.data(), _vertexBytes.size(), namePrefix + "VertexBuffer");
        if (vertexBuffer == nullptr)
        {
            return fail(outError, "Failed to upload " + namePrefix + "'s vertex buffer");
        }
        _vertexBuffer = vertexBuffer->getHandle();

        VkmBuffer* indexBuffer = createAndUploadBuffer(
            driver, _indices.data(), _indices.size() * sizeof(uint32_t), namePrefix + "IndexBuffer");
        if (indexBuffer == nullptr)
        {
            destroy(driver);
            return fail(outError, "Failed to upload " + namePrefix + "'s index buffer");
        }
        _indexBuffer = indexBuffer->getHandle();

        _vertexPoolSlot = bindlessManager->registerBuffer(_vertexBuffer, VkmBindlessArrayType::Buffer);
        _indexPoolSlot = bindlessManager->registerBuffer(_indexBuffer, VkmBindlessArrayType::IndexBuffer);
        if (_vertexPoolSlot == INVALID_VALUE32 || _indexPoolSlot == INVALID_VALUE32)
        {
            destroy(driver);
            return fail(outError, "The bindless buffer arrays are exhausted while registering " + namePrefix);
        }

        // The GPU copy is authoritative from here on; nothing re-reads the staging vectors.
        _vertexBytes.clear();
        _vertexBytes.shrink_to_fit();
        _indices.clear();
        _indices.shrink_to_fit();
        return true;
    }

    void VkmSceneGeometryPool::destroy(VkmDriverBase* driver)
    {
        VKM_ASSERT(driver != nullptr, "VkmSceneGeometryPool::destroy requires a driver");

        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        if (bindlessManager != nullptr)
        {
            if (_vertexPoolSlot != INVALID_VALUE32)
            {
                bindlessManager->unregisterBuffer(_vertexPoolSlot, VkmBindlessArrayType::Buffer);
            }
            if (_indexPoolSlot != INVALID_VALUE32)
            {
                bindlessManager->unregisterBuffer(_indexPoolSlot, VkmBindlessArrayType::IndexBuffer);
            }
        }
        _vertexPoolSlot = INVALID_VALUE32;
        _indexPoolSlot = INVALID_VALUE32;

        VkmDeferredResourceReclaimer* reclaimer = driver->getDeferredReclaimer();
        if (reclaimer != nullptr)
        {
            if (_vertexBuffer != VKM_INVALID_RESOURCE_HANDLE)
            {
                reclaimer->requestRelease(_vertexBuffer);
            }
            if (_indexBuffer != VKM_INVALID_RESOURCE_HANDLE)
            {
                reclaimer->requestRelease(_indexBuffer);
            }
        }
        _vertexBuffer = VKM_INVALID_RESOURCE_HANDLE;
        _indexBuffer = VKM_INVALID_RESOURCE_HANDLE;

        _vertexBytes.clear();
        _indices.clear();
        _indexCount = 0;
    }
} // namespace vkm
