// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <glm/vec3.hpp>

#include <string>

namespace vkm
{
    constexpr const uint32_t BACK_BUFFER_COUNT = 3;
    constexpr const uint32_t FRAME_BUFFER_COUNT = 3;
    // Vulkan allows the driver to create more swapchain images than the requested
    // minImageCount (Mesa's X11 WSI does, e.g. under lavapipe); per-image storage is
    // sized to this upper bound while FRAME_BUFFER_COUNT stays the requested count.
    constexpr const uint32_t MAX_BACK_BUFFER_COUNT = 8;

    enum class VkmResourceType : uint8_t
    {
        Texture = 0,
        Buffer = 1,
        StagingBuffer = 2,
        Sampler = 3,
        TextureView = 4,
        BufferView = 5,
        AccelerationStructure = 6,
        Count = 7,
        Undefined = Count,
    };

    // Display name for a resource category, shared by the render graph inspector and the
    // memory report.
    const char* vkmResourceTypeName(VkmResourceType type);

    enum class VkmResourcePoolType : uint8_t
    {
        Default = 0,
        Count = 1,
        Undefined = Count,
    };

    /*
    * @brief Resource handle
    */
    struct VkmResourceHandle
    {
        // Containers that mirror handle fields (e.g. VkmDriverResourceSubPool's
        // _freeIds/_generations) must use these aliases rather than hardcoding the
        // integer types.
        using IdType = uint64_t;
        using GenerationType = uint32_t;

        IdType id;
        VkmResourcePoolType poolType;
        VkmResourceType type;
        GenerationType generation = 0;

        constexpr const bool operator==(const VkmResourceHandle& other) const
        {
            return id == other.id && poolType == other.poolType && type == other.type && generation == other.generation;
        }
        constexpr const bool operator!=(const VkmResourceHandle& other) const
        {
            return !(*this == other);
        }
        const bool isValid() const
        {
            return (id != (IdType)-1);
        }
        const bool isPooledResource() const
        {
            return (poolType != VkmResourcePoolType::Undefined);
        }
    };
    constexpr const VkmResourceHandle VKM_INVALID_RESOURCE_HANDLE{(VkmResourceHandle::IdType)-1, VkmResourcePoolType::Undefined, VkmResourceType::Undefined};

    enum class VkmFormat : uint32_t
    {
        Undefined = 0,
        R8G8B8A8_UNORM = 1,
        R8G8B8A8_SRGB = 2,
        R8G8B8A8_UINT = 3,
        R8G8B8A8_SNORM = 4,
        R8G8B8A8_SINT = 5,
        R16G16B16A16_UNORM = 6,
        R16G16B16A16_SFLOAT = 7,
        R32G32B32A32_SFLOAT = 8,
        D32_SFLOAT = 9,
        D24_UNORM_S8_UINT = 10,
        D32_SFLOAT_S8_UINT = 11,
        BGRA8_UNORM = 12,
        BGRA8_SRGB = 13,
        // Sentinel: a pipeline color attachment that adopts the swapchain's color format.
        // Resolved to a concrete format in VkmDriverBase::newPipelineState() before any backend
        // consumes it -- never reaches the getMTLPixelFormat/toVkFormat/toWGPUTextureFormat converters.
        Swapchain = 14,
    };

    inline bool hasDepth(const VkmFormat format)
    {
        return (format == VkmFormat::D32_SFLOAT || format == VkmFormat::D24_UNORM_S8_UINT || format == VkmFormat::D32_SFLOAT_S8_UINT);
    }

    inline bool hasStencil(const VkmFormat format)
    {
        return (format == VkmFormat::D24_UNORM_S8_UINT || format == VkmFormat::D32_SFLOAT_S8_UINT);
    }

    enum class VkmResourceCreateInfo : uint32_t
    {
        AllowTransferSrc = 0x00000001,
        AllowTransferDst = 0x00000002,
        AllowShaderRead = 0x00000004,
        AllowShaderWrite = 0x00000008,
        AllowColorAttachment = 0x00000010,
        AllowDepthStencilAttachment = 0x00000020,
        AllowPresent = 0x00000040,
        ExternalHandleOwner = 0x00000080,
        DeferredCreation = 0x00000100,
        // Draw/dispatch argument buffer: the GPU-driven scene path has a compute pass write
        // VkmDrawIndirectArguments records that the draw then fetches from this same buffer.
        AllowIndirectBuffer = 0x00000200,
        /*
        * Read in place by an acceleration structure build -- vertex and index data a BLAS is built
        * from, so tracing a scene duplicates none of its geometry.
        * On Vulkan this also forces the committed allocation path. A build input needs both
        * `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` and
        * `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR` on the VkBuffer it
        * reads, and a sub-allocated buffer inherits the pool block's usage instead.
        */
        AllowAccelerationStructureInput = 0x00000400,

        AllowShaderReadWrite = AllowShaderRead | AllowShaderWrite,
    };

    VkmResourceCreateInfo operator|(VkmResourceCreateInfo lhs, VkmResourceCreateInfo rhs);
    uint32_t operator&(VkmResourceCreateInfo lhs, VkmResourceCreateInfo rhs);

    /*
    * @brief How one render subgraph touches one resource, declared alongside the resource so
    * VkmRenderGraph::compile() can derive the dependencies between subgraphs and place barriers
    * for them.
    *
    * @details A closed enum rather than a bitfield, deliberately. A bitfield would let a caller
    * write ShaderSampledRead | ColorAttachmentWrite, which has no single valid Vulkan image
    * layout -- exactly the impossible state that forced the engine's earlier barrier entry
    * points to be single-purpose. Combinations that *are* representable get their own entry
    * (ShaderStorageReadWrite); a subgraph that genuinely needs two accesses on one resource
    * declares it twice, and the analysis rejects the pair if their layouts disagree.
    *
    * Each backend maps these to its own vocabulary in its own translation unit: Vulkan to
    * (VkPipelineStageFlags2, VkAccessFlags2, VkImageLayout), Metal to MTLStages, WebGPU to
    * nothing at all.
    */
    enum class VkmResourceAccess : uint8_t
    {
        // No access. Used as the source of a resource's first access in a frame, where the real
        // prior state is backend knowledge rather than something the graph declared.
        None = 0,

        // Fetched as draw or dispatch arguments (VkmCommandBufferBase::drawIndirectCount).
        IndirectArgument,

        // Read as a uniform/constant buffer. Distinct from ShaderStorageRead because Vulkan
        // scopes the two with different access bits, and a barrier naming the storage one does
        // not make a write visible to a uniform read.
        ConstantBufferRead,

        // Read through a sampler.
        ShaderSampledRead,
        // Read, written, or both through a storage buffer/image binding.
        ShaderStorageRead,
        ShaderStorageWrite,
        ShaderStorageReadWrite,

        // Written as a render pass attachment. Derived from a graphics subgraph's
        // VkmFrameBufferDescriptor rather than declared by hand.
        ColorAttachmentWrite,
        DepthStencilAttachmentWrite,
        // Depth bound read-only, so it can be sampled by the same pass that tests against it.
        DepthStencilAttachmentRead,

        // Source or destination of a copy.
        TransferRead,
        TransferWrite,

        // Read as build input (vertex/index/instance data), or written as the built structure.
        AccelerationStructureBuildRead,
        AccelerationStructureBuildWrite,
        // Traversed by a ray query.
        AccelerationStructureShaderRead,

        // Handed to the swapchain.
        Present,

        Count,
    };

    /*
    * @brief Which pipeline the work doing the access runs on.
    *
    * @details A shader read does not say by itself whether it happens in a draw or in a
    * dispatch, and naming every stage in the access enum would double it. The declaring
    * subgraph already knows -- a graphics subgraph can only draw, a compute one can only
    * dispatch, a transfer one can only copy -- so the scope is derived from the subgraph type
    * and paired with the access. Accesses that carry their own stage regardless (attachments,
    * indirect fetch, transfers, acceleration structure builds) ignore it.
    */
    enum class VkmPipelineScope : uint8_t
    {
        Graphics,
        Compute,
        Transfer,
        All, // unknown, or work outside the render graph -- the conservative answer
    };

    // Whether an access writes the resource. Read-after-read needs no barrier; everything else
    // does, which is what makes this the core of the dependency analysis.
    bool vkmIsWriteAccess(VkmResourceAccess access);

    // Display name, for the render graph inspector and for validation messages.
    const char* vkmResourceAccessName(VkmResourceAccess access);

    // Every mip level / array layer of a texture, the default a declaration carries unless it
    // names a narrower range.
    constexpr const uint32_t VKM_ALL_REMAINING_SUBRESOURCES = ~0u;

    /*
    * @brief The mip levels and array layers of a texture a declaration covers. Ignored for
    * buffers and acceleration structures, which have no subresources.
    *
    * @details A probe atlas updated one cell at a time and a cubemap uploaded one face at a
    * time both leave the rest of the texture in whatever state it already had, so a barrier
    * that covered the whole image would either transition layers it must not or record a state
    * the tracker cannot honour.
    */
    struct VkmSubresourceRange
    {
        uint32_t _baseMipLevel = 0;
        uint32_t _mipLevelCount = VKM_ALL_REMAINING_SUBRESOURCES;
        uint32_t _baseArrayLayer = 0;
        uint32_t _arrayLayerCount = VKM_ALL_REMAINING_SUBRESOURCES;

        inline bool operator==(const VkmSubresourceRange& other) const noexcept
        {
            return _baseMipLevel == other._baseMipLevel && _mipLevelCount == other._mipLevelCount &&
                   _baseArrayLayer == other._baseArrayLayer && _arrayLayerCount == other._arrayLayerCount;
        }
        inline bool operator!=(const VkmSubresourceRange& other) const noexcept { return !(*this == other); }

        // Whether this range names every subresource a texture of these dimensions has -- the
        // case a backend can serve with one whole-image barrier.
        bool coversAll(uint32_t numMipLevels, uint32_t numArrayLayers) const;
    };

    // One "this subgraph touches that resource this way" statement, the unit
    // VkmRenderSubGraph::addReferencedResource records and the barrier analysis consumes.
    struct VkmResourceAccessDeclaration
    {
        VkmResourceHandle _handle = VKM_INVALID_RESOURCE_HANDLE;
        VkmResourceAccess _access = VkmResourceAccess::None;
        VkmSubresourceRange _range;
    };

    // Threadgroup width the engine's own 1D scene compute passes declare as [numthreads(N, 1, 1)],
    // so their dispatch sites can derive a group count from an item count (see VkmScene::recordCull).
    // A convention for those passes, not a constraint on compute shaders in general: a shader's real
    // [numthreads(...)] is read out of its compiled SPIR-V by vkm-compiler and carried in
    // VkmShaderCacheHeader::threadGroupSize, which is what Metal dispatches with.
    inline constexpr uint32_t kVkmComputeThreadGroupSizeX = 64;

    /*
    * @brief One non-indexed indirect draw record.
    * @details Byte-identical to VkDrawIndirectCommand, MTLDrawPrimitivesIndirectArguments and
    * WebGPU's non-indexed indirect layout, so the culling compute shader writes one format for
    * every backend. An all-zero record draws nothing on all three, which is what culled slots
    * become on the backends that have no GPU-side draw count.
    */
    struct VkmDrawIndirectArguments
    {
        uint32_t _vertexCount = 0;
        uint32_t _instanceCount = 0;
        uint32_t _firstVertex = 0;
        // Carries the VkmObjectData index rather than a real instance offset; the vertex shader
        // reads it back as SV_InstanceID, which is what lets the draw path push no constants.
        uint32_t _firstInstance = 0;
    };
    static_assert(sizeof(VkmDrawIndirectArguments) == 16, "VkmDrawIndirectArguments must match the native indirect argument layouts");

    /*
    * @brief One indexed indirect draw record.
    * @details Byte-identical to VkDrawIndexedIndirectCommand,
    * MTLDrawIndexedPrimitivesIndirectArguments and WebGPU's indexed indirect layout. _vertexOffset
    * is SIGNED on all three -- the one field a mirror written as five uint32s gets wrong.
    * Nothing in the engine draws with this layout: an indexed draw needs a bound index buffer, and
    * the engine has none, indices being pulled in the vertex shader from a bindless storage buffer.
    * It lives beside its non-indexed sibling because both layouts have to be describable before a
    * draw call can say which one it is given.
    */
    struct VkmDrawIndexedIndirectArguments
    {
        uint32_t _indexCount = 0;
        uint32_t _instanceCount = 0;
        uint32_t _firstIndex = 0;
        int32_t _vertexOffset = 0; // signed: added to every index before the vertex fetch
        uint32_t _firstInstance = 0;
    };
    static_assert(sizeof(VkmDrawIndexedIndirectArguments) == 20,
                  "VkmDrawIndexedIndirectArguments must match the native indexed indirect argument layouts");

    /*
    * @brief Which record layout an indirect argument buffer holds.
    * @details Supplied alongside the buffer to VkmCommandBufferBase::drawIndirectCount so the
    * record stride comes from a layout the caller declared rather than one each backend assumed.
    */
    enum class VkmIndirectArgumentLayout : uint8_t
    {
        NonIndexed = 0, // VkmDrawIndirectArguments
        Indexed = 1,    // VkmDrawIndexedIndirectArguments
    };

    // Byte step between consecutive records of `layout`.
    uint32_t vkmGetIndirectArgumentStride(VkmIndirectArgumentLayout layout);

    struct VkmResourceInfo
    {
        VkmResourceCreateInfo _flags;
        const char* _debugName = nullptr;
    };

    /*
    * @brief Per-resource-allocation memory bookkeeping, mirroring vkm::MemoryTracker's
    * CPU-side tag pattern (include/vkm/base/memory.h) but for individual GPU resource
    * allocations rather than call-site-aggregated heap allocations.
    */
    struct VkmResourceMemoryTag
    {
        uint64_t requestedSize = 0;
        uint64_t allocatedSize = 0;
        uint32_t alignment = 0;
        std::string name;
        std::string metadata;
        VkmResourceType type = VkmResourceType::Undefined; // Undefined marks an empty/unset tag
    };

    /*
    * @brief Aggregated, persistent running totals for one VkmResourceType category.
    * @details Unlike VkmResourceMemoryTag, which goes away when its handle is released, this
    * decrements on release rather than resetting.
    */
    struct VkmResourceCategoryUsage
    {
        uint64_t totalRequestedBytes = 0;
        uint64_t totalAllocatedBytes = 0;
        uint32_t liveCount = 0;
    };

    /*
    * @brief What the graphics API itself reports about device memory, as opposed to the
    * per-resource totals the engine accumulates in VkmRenderResourcePool.
    * @details The gap between the two is the allocator's own cost: block padding, alignment,
    * driver-side bookkeeping. Availability is per-backend -- WebGPU exposes no memory
    * introspection, so it leaves every field zero and both flags false.
    */
    struct VkmGpuMemoryStats
    {
        uint64_t _deviceAllocatedBytes = 0; // what the API says is really allocated on the device
        uint64_t _deviceBudgetBytes = 0;    // working-set budget, 0 when the backend can't report one
        uint64_t _poolReservedBytes = 0;    // bytes the engine's own suballocator holds in blocks/heaps
        uint64_t _poolUsedBytes = 0;        // bytes handed out from within those blocks
        bool _hasDeviceStats = false;
        bool _hasPoolStats = false;
    };

    /*
    * @brief Whether a resource gets its own allocation or is suballocated from a shared one.
    * @details A request, not a guarantee. Heap downgrades to Committed where the backend cannot
    * honour it: WebGPU exposes no placement API, a host-writable resource cannot enter a
    * device-private pool, and Vulkan falls back when no pool block has room.
    *
    * Auto leaves the choice to the backend's allocator, which has information a call site does
    * not: on Vulkan, VMA queries VkMemoryDedicatedRequirements per resource and weighs it
    * against block occupancy and maxMemoryAllocationCount. Name a placement explicitly only
    * when the resource's use -- not its size -- requires it.
    *
    * What the shared allocation is differs per backend and resource kind: a sub-range of one
    * VkBuffer, a VMA-suballocated VkDeviceMemory, or an MTLHeap. Heap asks "may this share its
    * backing allocation", not "which API object backs it".
    */
    enum class VkmMemoryPlacementHint : uint8_t
    {
        Auto = 0,      // let the backend's allocator decide
        Committed = 1, // its own allocation, never suballocated from an engine pool/heap
        Heap = 2,      // suballocate from the engine's shared pool/heap where one exists
    };

    /*
    * @brief Which side is allowed to write a buffer's memory directly.
    * @details A request, not a guarantee: VkmBuffer::isHostWritable() reports what the backend
    * actually allocated. HostWrite enables VkmBuffer::map()/unmap() and the direct uploadToBuffer
    * path; DeviceLocal, the default, is GPU-only memory reachable from the CPU only through a
    * staging copy.
    * HostWrite always takes the committed path, both backends' suballocation pools being backed by
    * device-private memory, so a host-writable buffer cannot be placed in one.
    */
    enum class VkmMemoryAccessHint : uint8_t
    {
        DeviceLocal = 0,
        HostWrite = 1,
    };

    /*
    * @brief How a texture (or a view of one) is addressed by the shader.
    * @details Auto infers a plain 2D image, or a 2D array once _numArrayLayers > 1. Cube is the one
    * case that inference cannot express, a cubemap and a 6-layer 2D array being the same
    * allocation described two different ways.
    */
    enum class VkmTextureType : uint8_t
    {
        Auto = 0, // 2D, or a 2D array when _numArrayLayers > 1
        Cube = 1, // requires _numArrayLayers == 6, ordered +X, -X, +Y, -Y, +Z, -Z
    };

    // The face count and face order every cube texture uses. The order is the one Vulkan,
    // Metal and D3D all agree on, and is what array layer N of a cube texture means.
    inline constexpr uint32_t kVkmCubeFaceCount = 6;

    /*
    * @brief How VkmDriverBase::uploadToTexture / uploadToBuffer move bytes into a resource.
    * @details Auto is what callers should use: it takes the direct CPU write when the destination's
    * memory allows one (VkmTexture::isHostWritable / VkmBuffer::isHostWritable, both decided at
    * creation) and the staging-buffer copy otherwise. The direct write skips the staging
    * allocation, the command buffer, the queue submit and the wait.
    * The explicit modes are for benchmarking and for tests that need one specific path.
    * ForceHostCopy on a resource whose memory cannot take one warns and falls back to staging; the
    * resulting bytes are identical either way.
    */
    enum class VkmResourceUploadMode : uint8_t
    {
        Auto = 0,
        ForceStaging = 1,
        ForceHostCopy = 2,
    };

    struct VkmTextureInfo : public VkmResourceInfo
    {
        glm::uvec3 _extent;
        uint32_t _numMipLevels;
        uint32_t _numArrayLayers;
        VkmFormat _format;
        VkmTextureType _type = VkmTextureType::Auto;
        VkmMemoryPlacementHint _placementHint = VkmMemoryPlacementHint::Auto;
    };

    // Display name for a pixel format, shared by the render graph inspector and the texture browser.
    const char* vkmFormatName(VkmFormat format);

    // Bytes per texel for uncompressed formats; 0 for Undefined/unknown.
    uint32_t vkmBytesPerTexel(VkmFormat format);

    /*
    * @brief Best-effort estimate of a texture's base-mip-level byte footprint: extent x array
    * layers x bytes-per-texel for the format.
    * @details Does not sum the full mip chain -- a rough size estimate is all this is for, not an
    * exact GPU byte count. Used as VkmResourceMemoryTag's requestedSize for textures on every
    * backend, Metal and WebGPU having no format-size introspection API of their own.
    * @param info Texture description.
    * @return The estimated byte size.
    */
    uint64_t computeTextureByteSize(const VkmTextureInfo& info);

    struct VkmBufferInfo : public VkmResourceInfo
    {
        uint64_t _size;
        VkmMemoryPlacementHint _placementHint = VkmMemoryPlacementHint::Auto;
        VkmMemoryAccessHint _accessHint = VkmMemoryAccessHint::DeviceLocal;
    };

    struct VkmStagingBufferInfo : public VkmResourceInfo
    {
        uint64_t _size;
    };

    enum class VkmFilterMode : uint8_t
    {
        Nearest = 0,
        Linear = 1,
    };

    enum class VkmMipmapMode : uint8_t
    {
        Nearest = 0,
        Linear = 1,
    };

    enum class VkmAddressMode : uint8_t
    {
        Repeat = 0,
        MirroredRepeat = 1,
        ClampToEdge = 2,
        ClampToBorder = 3,
    };

    enum class VkmCompareOp : uint8_t
    {
        Never = 0,
        Less = 1,
        Equal = 2,
        LessOrEqual = 3,
        Greater = 4,
        NotEqual = 5,
        GreaterOrEqual = 6,
        Always = 7,
    };

    struct VkmSamplerInfo : public VkmResourceInfo
    {
        VkmFilterMode _minFilter = VkmFilterMode::Linear;
        VkmFilterMode _magFilter = VkmFilterMode::Linear;
        VkmMipmapMode _mipmapMode = VkmMipmapMode::Linear;
        VkmAddressMode _addressModeU = VkmAddressMode::ClampToEdge;
        VkmAddressMode _addressModeV = VkmAddressMode::ClampToEdge;
        VkmAddressMode _addressModeW = VkmAddressMode::ClampToEdge;
        float _maxAnisotropy = 1.0f;
        bool _compareEnable = false;
        VkmCompareOp _compareOp = VkmCompareOp::Never;
        float _minLod = 0.0f;
        float _maxLod = 1000.0f;
    };

    struct VkmTextureViewInfo : public VkmResourceInfo
    {
        VkmResourceHandle _texture;
        VkmFormat _format = VkmFormat::Undefined;
        uint32_t _baseMipLevel = 0;
        uint32_t _numMipLevels = UINT32_MAX;
        uint32_t _baseArrayLayer = 0;
        uint32_t _numArrayLayers = UINT32_MAX;
        VkmTextureType _type = VkmTextureType::Auto;
    };

    struct VkmBufferViewInfo : public VkmResourceInfo
    {
        VkmResourceHandle _buffer;
        uint64_t _offset = 0;
        uint64_t _size = UINT64_MAX;
        VkmFormat _format = VkmFormat::Undefined;
    };

    enum class VkmCommandQueueType : uint8_t
    {
        Graphics = 0,
        Compute = 1,
        Transfer = 2,
        Count = 3,
        Undefined = Count,
    };

    enum class VkmCommandQueueTypeBits : uint32_t
    {
        Graphics = 1 << 0,
        Compute = 1 << 1,
        Transfer = 1 << 2,
    };

    using VKM_COMMAND_BUFFER_HANDLE = void*;
} // namespace vkm

template <>
struct std::hash<vkm::VkmResourceHandle>
{
    std::size_t operator()(const vkm::VkmResourceHandle& handle) const noexcept
    {
        return std::hash<uint64_t>()(handle.id) ^ std::hash<uint8_t>()(static_cast<uint8_t>(handle.poolType)) ^ std::hash<uint8_t>()(static_cast<uint8_t>(handle.type)) ^ std::hash<uint32_t>()(handle.generation);
    }
};