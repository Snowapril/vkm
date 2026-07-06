// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_util.h>

namespace vkm
{
    std::string toStdString(WGPUStringView view)
    {
        if (view.data == nullptr || view.length == 0)
        {
            return std::string();
        }
        return std::string(view.data, view.length);
    }

    WGPULoadOp toWGPULoadOp(VkmLoadAction loadAction)
    {
        switch (loadAction)
        {
            case VkmLoadAction::Load:     return WGPULoadOp_Load;
            case VkmLoadAction::Clear:    return WGPULoadOp_Clear;
            case VkmLoadAction::DontCare: return WGPULoadOp_Load; // WebGPU has no "don't care" load op
            default: VKM_ASSERT(false, "Invalid load action"); return WGPULoadOp_Load;
        }
    }

    WGPUStoreOp toWGPUStoreOp(VkmStoreAction storeAction)
    {
        switch (storeAction)
        {
            case VkmStoreAction::Store:    return WGPUStoreOp_Store;
            case VkmStoreAction::DontCare: return WGPUStoreOp_Discard;
            default: VKM_ASSERT(false, "Invalid store action"); return WGPUStoreOp_Store;
        }
    }

    WGPUTextureFormat toWGPUTextureFormat(VkmFormat format)
    {
        switch (format)
        {
            case VkmFormat::R8G8B8A8_UNORM:     return WGPUTextureFormat_RGBA8Unorm;
            case VkmFormat::R8G8B8A8_SRGB:       return WGPUTextureFormat_RGBA8UnormSrgb;
            case VkmFormat::R8G8B8A8_UINT:       return WGPUTextureFormat_RGBA8Uint;
            case VkmFormat::R8G8B8A8_SNORM:      return WGPUTextureFormat_RGBA8Snorm;
            case VkmFormat::R8G8B8A8_SINT:       return WGPUTextureFormat_RGBA8Sint;
            case VkmFormat::R16G16B16A16_UNORM:  return WGPUTextureFormat_RGBA16Unorm;
            case VkmFormat::R16G16B16A16_SFLOAT: return WGPUTextureFormat_RGBA16Float;
            case VkmFormat::R32G32B32A32_SFLOAT: return WGPUTextureFormat_RGBA32Float;
            case VkmFormat::D32_SFLOAT:          return WGPUTextureFormat_Depth32Float;
            case VkmFormat::D24_UNORM_S8_UINT:   return WGPUTextureFormat_Depth24PlusStencil8;
            case VkmFormat::D32_SFLOAT_S8_UINT:  return WGPUTextureFormat_Depth32FloatStencil8;
            case VkmFormat::B8G8R8A8_UNORM:      return WGPUTextureFormat_BGRA8Unorm;
            default: VKM_ASSERT(false, "Unsupported texture format for WebGPU backend"); return WGPUTextureFormat_Undefined;
        }
    }

    VkmFormat fromWGPUTextureFormat(WGPUTextureFormat format)
    {
        switch (format)
        {
            case WGPUTextureFormat_RGBA8Unorm:     return VkmFormat::R8G8B8A8_UNORM;
            case WGPUTextureFormat_RGBA8UnormSrgb: return VkmFormat::R8G8B8A8_SRGB;
            case WGPUTextureFormat_RGBA8Uint:      return VkmFormat::R8G8B8A8_UINT;
            case WGPUTextureFormat_RGBA8Snorm:     return VkmFormat::R8G8B8A8_SNORM;
            case WGPUTextureFormat_RGBA8Sint:      return VkmFormat::R8G8B8A8_SINT;
            case WGPUTextureFormat_BGRA8Unorm:     return VkmFormat::B8G8R8A8_UNORM;
            case WGPUTextureFormat_RGBA16Unorm:    return VkmFormat::R16G16B16A16_UNORM;
            case WGPUTextureFormat_RGBA16Float:    return VkmFormat::R16G16B16A16_SFLOAT;
            case WGPUTextureFormat_RGBA32Float:    return VkmFormat::R32G32B32A32_SFLOAT;
            case WGPUTextureFormat_Depth32Float:   return VkmFormat::D32_SFLOAT;
            case WGPUTextureFormat_Depth24PlusStencil8: return VkmFormat::D24_UNORM_S8_UINT;
            case WGPUTextureFormat_Depth32FloatStencil8: return VkmFormat::D32_SFLOAT_S8_UINT;
            default: VKM_ASSERT(false, "Unsupported WGPUTextureFormat for VkmFormat conversion"); return VkmFormat::Undefined;
        }
    }

    void logWGPUUncapturedError(WGPUDevice const*, WGPUErrorType type, WGPUStringView message, void*, void*)
    {
        if (type == WGPUErrorType_NoError)
        {
            return;
        }
        VKM_DEBUG_ERROR(fmt::format("WebGPU uncaptured error ({}): {}", (int)type, toStdString(message)).c_str());
    }
} // namespace vkm
