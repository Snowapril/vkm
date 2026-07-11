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

    uint64_t computeTextureByteSize(const VkmTextureInfo& info)
    {
        uint64_t bytesPerTexel = 0;
        switch (info._format)
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
                bytesPerTexel = 4;
                break;
            case VkmFormat::R16G16B16A16_UNORM:
            case VkmFormat::R16G16B16A16_SFLOAT:
            case VkmFormat::D32_SFLOAT_S8_UINT: // packed/padded to 8 bytes on GPU
                bytesPerTexel = 8;
                break;
            case VkmFormat::R32G32B32A32_SFLOAT:
                bytesPerTexel = 16;
                break;
            case VkmFormat::Undefined:
            default:
                bytesPerTexel = 0;
                break;
        }
        return (uint64_t)info._extent.x * info._extent.y * info._extent.z * info._numArrayLayers * bytesPerTexel;
    }
} // namespace vkm