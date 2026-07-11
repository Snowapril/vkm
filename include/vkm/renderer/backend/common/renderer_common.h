// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <glm/vec3.hpp>

namespace vkm
{
    constexpr const uint32_t BACK_BUFFER_COUNT = 3;
    constexpr const uint32_t FRAME_BUFFER_COUNT = 3;

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
        uint64_t id;
        VkmResourcePoolType poolType;
        VkmResourceType type;
        uint32_t generation = 0;

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
            return (id != (uint64_t)-1);
        }
        const bool isPooledResource() const
        {
            return (poolType != VkmResourcePoolType::Undefined);
        }
    };
    constexpr const VkmResourceHandle VKM_INVALID_RESOURCE_HANDLE{(uint64_t)-1, VkmResourcePoolType::Undefined, VkmResourceType::Undefined};

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

        AllowShaderReadWrite = AllowShaderRead | AllowShaderWrite,
    };

    VkmResourceCreateInfo operator|(VkmResourceCreateInfo lhs, VkmResourceCreateInfo rhs);
    uint32_t operator&(VkmResourceCreateInfo lhs, VkmResourceCreateInfo rhs);

    enum class VkmResourceUsageBits : uint32_t
    {
    };

    struct VkmResourceInfo
    {
        VkmResourceCreateInfo _flags;
        VkmResourceUsageBits _usage;
    };

    enum class VkmMemoryPlacementHint : uint8_t
    {
        Auto = 0,
        ForceCommitted = 1,
        ForcePooled = 2,
    };

    struct VkmTextureInfo : public VkmResourceInfo
    {
        glm::uvec3 _extent;
        uint32_t _numMipLevels;
        uint32_t _numArrayLayers;
        VkmFormat _format;
        VkmMemoryPlacementHint _placementHint = VkmMemoryPlacementHint::Auto;
    };

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