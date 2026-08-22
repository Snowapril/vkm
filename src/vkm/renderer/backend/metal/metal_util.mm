// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/metal/metal_util.h>

namespace vkm
{
    std::string mtlErrorToString(NSError* error)
    {
        if (error == nil || error.localizedDescription == nil || error.localizedDescription.UTF8String == nullptr)
        {
            return "unknown error";
        }
        return std::string(error.localizedDescription.UTF8String);
    }

    bool mtlCheckObject(id object, NSError* error, const char* msg)
    {
        if (object == nil)
        {
            VKM_DEBUG_ERROR(fmt::format("{}: {}", msg, mtlErrorToString(error)).c_str());
            return false;
        }
        return true;
    }

    MTLPixelFormat getMTLPixelFormat(VkmFormat format)
    {
        switch (format)
        {
            case VkmFormat::R8G8B8A8_UNORM:      return MTLPixelFormatRGBA8Unorm;
            case VkmFormat::R8G8B8A8_SRGB:       return MTLPixelFormatRGBA8Unorm_sRGB;
            case VkmFormat::R8G8B8A8_UINT:       return MTLPixelFormatRGBA8Uint;
            case VkmFormat::R8G8B8A8_SNORM:      return MTLPixelFormatRGBA8Snorm;
            case VkmFormat::R8G8B8A8_SINT:       return MTLPixelFormatRGBA8Sint;
            case VkmFormat::R16G16B16A16_UNORM:  return MTLPixelFormatRGBA16Unorm;
            case VkmFormat::R16G16B16A16_SFLOAT: return MTLPixelFormatRGBA16Float;
            case VkmFormat::R32G32B32A32_SFLOAT: return MTLPixelFormatRGBA32Float;
            case VkmFormat::D32_SFLOAT:          return MTLPixelFormatDepth32Float;
            case VkmFormat::D24_UNORM_S8_UINT:   return MTLPixelFormatDepth24Unorm_Stencil8;
            case VkmFormat::D32_SFLOAT_S8_UINT:  return MTLPixelFormatDepth32Float_Stencil8;
            case VkmFormat::BGRA8_UNORM:         return MTLPixelFormatBGRA8Unorm;
            case VkmFormat::BGRA8_SRGB:          return MTLPixelFormatBGRA8Unorm_sRGB;
            case VkmFormat::Undefined:
            default:                             return MTLPixelFormatInvalid;
        }
    }
} // namespace vkm
