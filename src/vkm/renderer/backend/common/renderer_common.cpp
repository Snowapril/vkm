// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/renderer_common.h>

namespace vkm
{
    VkmResourceCreateInfo operator|(VkmResourceCreateInfo lhs, VkmResourceCreateInfo rhs)
    {
        return VkmResourceCreateInfo(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    };

    uint32_t operator&(VkmResourceCreateInfo lhs, VkmResourceCreateInfo rhs)
    {
        return static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs);
    };

    const char* vkmResourceTypeName(VkmResourceType type)
    {
        switch (type)
        {
            case VkmResourceType::Texture:       return "Texture";
            case VkmResourceType::Buffer:        return "Buffer";
            case VkmResourceType::StagingBuffer: return "StagingBuffer";
            case VkmResourceType::Sampler:       return "Sampler";
            case VkmResourceType::TextureView:   return "TextureView";
            case VkmResourceType::BufferView:    return "BufferView";
            default:                             return "Undefined";
        }
    }

    uint32_t vkmGetIndirectArgumentStride(VkmIndirectArgumentLayout layout)
    {
        switch (layout)
        {
            case VkmIndirectArgumentLayout::NonIndexed: return sizeof(VkmDrawIndirectArguments);
            case VkmIndirectArgumentLayout::Indexed:    return sizeof(VkmDrawIndexedIndirectArguments);
            default:
                VKM_ASSERT(false, "Unhandled VkmIndirectArgumentLayout");
                return sizeof(VkmDrawIndirectArguments);
        }
    }

    uint32_t vkmBytesPerTexel(VkmFormat format)
    {
        switch (format)
        {
            case VkmFormat::R8G8B8A8_UNORM:
            case VkmFormat::R8G8B8A8_SRGB:
            case VkmFormat::R8G8B8A8_UINT:
            case VkmFormat::R8G8B8A8_SNORM:
            case VkmFormat::R8G8B8A8_SINT:
            case VkmFormat::BGRA8_UNORM:
            case VkmFormat::BGRA8_SRGB:
            case VkmFormat::D32_SFLOAT:
            case VkmFormat::D24_UNORM_S8_UINT:
                return 4;
            case VkmFormat::R16G16B16A16_UNORM:
            case VkmFormat::R16G16B16A16_SFLOAT:
            case VkmFormat::D32_SFLOAT_S8_UINT: // packed/padded to 8 bytes on GPU
                return 8;
            case VkmFormat::R32G32B32A32_SFLOAT:
                return 16;
            case VkmFormat::Undefined:
            default:
                return 0;
        }
    }

    uint64_t computeTextureByteSize(const VkmTextureInfo& info)
    {
        return (uint64_t)info._extent.x * info._extent.y * info._extent.z * info._numArrayLayers * vkmBytesPerTexel(info._format);
    }
} // namespace vkm