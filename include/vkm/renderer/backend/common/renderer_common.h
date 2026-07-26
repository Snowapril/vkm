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
        Count = 6,
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
    * @details
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

        AllowShaderReadWrite = AllowShaderRead | AllowShaderWrite,
    };

    VkmResourceCreateInfo operator|(VkmResourceCreateInfo lhs, VkmResourceCreateInfo rhs);
    uint32_t operator&(VkmResourceCreateInfo lhs, VkmResourceCreateInfo rhs);

    enum class VkmResourceUsageBits : uint32_t
    {
    };

    // Threadgroup width every engine compute shader declares as [numthreads(N, 1, 1)]. It is an
    // engine constant rather than PSO state because Metal needs threadsPerThreadgroup at dispatch
    // time and MTLComputePipelineState cannot be asked what the shader declared.
    inline constexpr uint32_t kVkmComputeThreadGroupSizeX = 64;

    /*
    * @brief One non-indexed indirect draw record.
    *
    * Byte-identical to VkDrawIndirectCommand, MTLDrawPrimitivesIndirectArguments and WebGPU's
    * non-indexed indirect layout, so the culling compute shader writes one format for every
    * backend. An all-zero record draws nothing on all three, which is what culled slots become
    * on the backends that have no GPU-side draw count (see
    * VkmCommandBufferBase::drawIndirectCount).
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
    *
    * Byte-identical to VkDrawIndexedIndirectCommand, MTLDrawIndexedPrimitivesIndirectArguments and
    * WebGPU's indexed indirect layout. Note that _vertexOffset is SIGNED on all three -- it is the
    * one field a mirror written as five uint32s gets wrong.
    *
    * Nothing in the engine draws with this layout yet: an indexed draw needs a bound index buffer,
    * and the engine deliberately has none (indices are pulled in the vertex shader out of a
    * bindless storage buffer -- see VkmCommandBufferBase::drawIndirectCount). The record lives here
    * beside its non-indexed sibling because it is what a producing compute shader would write, and
    * because both layouts have to be describable before a draw call can say which one it is given.
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
    *
    * Supplied alongside the buffer to VkmCommandBufferBase::drawIndirectCount so the record stride
    * is derived from a layout the caller declared rather than assumed independently by each
    * backend.
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
        VkmResourceUsageBits _usage;
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
    * @brief Aggregated, persistent running totals for one VkmResourceType category. Unlike
    * VkmResourceMemoryTag (which goes away when its handle is released), this decrements on
    * release rather than resetting -- it is the meaningful historical/debugging signal,
    * mirroring MemoryTracker's aggregate-level intent.
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
    * @details The gap between the two is the allocator's own cost -- block padding,
    * alignment, driver-side bookkeeping -- which is exactly why both are worth showing side
    * by side. Availability is per-backend: WebGPU exposes no memory introspection at all, so
    * it leaves every field zero and both flags false.
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

    enum class VkmMemoryPlacementHint : uint8_t
    {
        Auto = 0,
        ForceCommitted = 1,
        ForcePooled = 2,
    };

    /*
    * @brief How a texture (or a view of one) is addressed by the shader.
    * @details Auto reproduces the inference every backend used before this enum existed --
    * a plain 2D image, or a 2D array once _numArrayLayers > 1 -- so leaving it unset keeps
    * existing behavior. Cube is the one case that inference cannot express, since a cubemap
    * and a 6-layer 2D array are the same allocation described two different ways.
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
    * @brief How VkmDriverBase::uploadToTexture moves pixels into a texture.
    * @details Auto is what callers should use: it takes the direct CPU write when the
    * destination's memory allows one (VkmTexture::isHostWritable, decided at creation from
    * what the backend actually allocated) and the staging-buffer copy otherwise. On a
    * unified-memory device the direct write skips the staging allocation, the command
    * buffer, the queue submit and the wait entirely; elsewhere nothing changes.
    *
    * The explicit modes exist for benchmarking and for tests that need to exercise one
    * specific path. ForceHostCopy on a texture whose memory cannot take one warns and falls
    * back to staging rather than failing -- the resulting pixels are identical either way,
    * so refusing would only make callers write their own fallback.
    */
    enum class VkmTextureUploadMode : uint8_t
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

    /*
    * @brief Best-effort estimate of a texture's base-mip-level byte footprint (extent x
    * array layers x bytes-per-texel for the format) -- does not sum the full mip chain, for
    * the same reason the Vulkan-backend's own VMA-dedicated-allocation heuristic doesn't
    * either (see shouldUseDedicatedTexture in vulkan_texture.cpp): a rough size estimate is
    * all that's needed here, not an exact GPU byte count. Used as VkmResourceMemoryTag's
    * requestedSize for textures, independent of backend (Metal/WebGPU have no format-size
    * introspection API of their own to compute this).
    */
    // Bytes per texel for uncompressed formats; 0 for Undefined/unknown.
    uint32_t vkmBytesPerTexel(VkmFormat format);

    uint64_t computeTextureByteSize(const VkmTextureInfo& info);

    struct VkmBufferInfo : public VkmResourceInfo
    {
        uint64_t _size;
        VkmMemoryPlacementHint _placementHint = VkmMemoryPlacementHint::Auto;
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