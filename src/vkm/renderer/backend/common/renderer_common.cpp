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
            case VkmResourceType::AccelerationStructure: return "AccelerationStructure";
            default:                             return "Undefined";
        }
    }

    bool vkmIsWriteAccess(VkmResourceAccess access)
    {
        switch (access)
        {
            case VkmResourceAccess::ShaderStorageWrite:
            case VkmResourceAccess::ShaderStorageReadWrite:
            case VkmResourceAccess::ColorAttachmentWrite:
            case VkmResourceAccess::DepthStencilAttachmentWrite:
            case VkmResourceAccess::TransferWrite:
            case VkmResourceAccess::AccelerationStructureBuildWrite:
                return true;
            default:
                return false;
        }
    }

    const char* vkmResourceAccessName(VkmResourceAccess access)
    {
        switch (access)
        {
            case VkmResourceAccess::None:                            return "None";
            case VkmResourceAccess::IndirectArgument:                return "IndirectArgument";
            case VkmResourceAccess::ConstantBufferRead:              return "ConstantBufferRead";
            case VkmResourceAccess::ShaderSampledRead:               return "ShaderSampledRead";
            case VkmResourceAccess::ShaderStorageRead:               return "ShaderStorageRead";
            case VkmResourceAccess::ShaderStorageWrite:              return "ShaderStorageWrite";
            case VkmResourceAccess::ShaderStorageReadWrite:          return "ShaderStorageReadWrite";
            case VkmResourceAccess::ColorAttachmentWrite:            return "ColorAttachmentWrite";
            case VkmResourceAccess::DepthStencilAttachmentWrite:     return "DepthStencilAttachmentWrite";
            case VkmResourceAccess::DepthStencilAttachmentRead:      return "DepthStencilAttachmentRead";
            case VkmResourceAccess::TransferRead:                    return "TransferRead";
            case VkmResourceAccess::TransferWrite:                   return "TransferWrite";
            case VkmResourceAccess::AccelerationStructureBuildRead:  return "AccelerationStructureBuildRead";
            case VkmResourceAccess::AccelerationStructureBuildWrite: return "AccelerationStructureBuildWrite";
            case VkmResourceAccess::AccelerationStructureShaderRead: return "AccelerationStructureShaderRead";
            case VkmResourceAccess::Present:                         return "Present";
            default:                                                 return "Unknown";
        }
    }

    bool VkmSubresourceRange::coversAll(uint32_t numMipLevels, uint32_t numArrayLayers) const
    {
        const uint32_t mipCount = (_mipLevelCount == VKM_ALL_REMAINING_SUBRESOURCES)
                                      ? (numMipLevels - _baseMipLevel) : _mipLevelCount;
        const uint32_t layerCount = (_arrayLayerCount == VKM_ALL_REMAINING_SUBRESOURCES)
                                        ? (numArrayLayers - _baseArrayLayer) : _arrayLayerCount;
        return _baseMipLevel == 0 && mipCount >= numMipLevels &&
               _baseArrayLayer == 0 && layerCount >= numArrayLayers;
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

    const char* vkmFormatName(VkmFormat format)
    {
        switch (format)
        {
            case VkmFormat::R8G8B8A8_UNORM:      return "R8G8B8A8_UNORM";
            case VkmFormat::R8G8B8A8_SRGB:       return "R8G8B8A8_SRGB";
            case VkmFormat::R8G8B8A8_UINT:       return "R8G8B8A8_UINT";
            case VkmFormat::R8G8B8A8_SNORM:      return "R8G8B8A8_SNORM";
            case VkmFormat::R8G8B8A8_SINT:       return "R8G8B8A8_SINT";
            case VkmFormat::R16G16B16A16_UNORM:  return "R16G16B16A16_UNORM";
            case VkmFormat::R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
            case VkmFormat::R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
            case VkmFormat::D32_SFLOAT:          return "D32_SFLOAT";
            case VkmFormat::D24_UNORM_S8_UINT:   return "D24_UNORM_S8_UINT";
            case VkmFormat::D32_SFLOAT_S8_UINT:  return "D32_SFLOAT_S8_UINT";
            case VkmFormat::BGRA8_UNORM:         return "BGRA8_UNORM";
            case VkmFormat::BGRA8_SRGB:          return "BGRA8_SRGB";
            case VkmFormat::Swapchain:           return "Swapchain";
            case VkmFormat::Undefined:
            default:                             return "Undefined";
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