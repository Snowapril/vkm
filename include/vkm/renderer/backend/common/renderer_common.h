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
        Count = 3,
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

        constexpr const bool operator==(const VkmResourceHandle& other) const
        {
            return id == other.id && poolType == other.poolType && type == other.type;
        }
        constexpr const bool operator!=(const VkmResourceHandle& other) const
        {
            return id != other.id || poolType != other.poolType || type != other.type;
        }
        const bool isValid() const
        {
            return (id != -1);
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
    };

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

    struct VkmTextureInfo : public VkmResourceInfo
    {
        glm::uvec3 _extent;
        uint32_t _numMipLevels;
        uint32_t _numArrayLayers;
        VkmFormat _format;
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
        return std::hash<uint64_t>()(handle.id) ^ std::hash<uint8_t>()(static_cast<uint8_t>(handle.poolType)) ^ std::hash<uint8_t>()(static_cast<uint8_t>(handle.type));
    }
};