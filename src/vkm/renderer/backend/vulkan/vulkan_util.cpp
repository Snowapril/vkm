// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vulkan/vulkan.h>

namespace vkm
{
namespace
{
// vulkan/vk_enum_string_helper.h is intentionally not used here: it is not vendored
// alongside the pinned Vulkan-Headers version and would instead resolve to whatever
// Vulkan SDK is installed system-wide, which can reference VkResult values unknown
// to the vendored <vulkan/vulkan_core.h> and fail to compile.
const char* vkResultToString(VkResult result)
{
    switch (result)
    {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_EVENT_SET:                      return "VK_EVENT_SET";
        case VK_EVENT_RESET:                    return "VK_EVENT_RESET";
        case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL:          return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN:                  return "VK_ERROR_UNKNOWN";
        case VK_ERROR_OUT_OF_POOL_MEMORY:       return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE:  return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_FRAGMENTATION:            return "VK_ERROR_FRAGMENTATION";
        case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_VALIDATION_FAILED_EXT:    return "VK_ERROR_VALIDATION_FAILED_EXT";
        default:                                return "VK_ERROR_UNKNOWN_RESULT";
    }
}
}

bool vkCheckResult(int result, const char* msg)
{
    if (result != VK_SUCCESS)
    {
        const std::string msgString = fmt::format("{}: {}", vkResultToString(static_cast<VkResult>(result)), msg);
        VKM_DEBUG_ERROR(msgString.c_str());
        return false;
    }
    return true;
}

VkFormat toVkFormat(VkmFormat format)
{
    switch (format)
    {
        case VkmFormat::R8G8B8A8_UNORM:     return VK_FORMAT_R8G8B8A8_UNORM;
        case VkmFormat::R8G8B8A8_SRGB:      return VK_FORMAT_R8G8B8A8_SRGB;
        case VkmFormat::R8G8B8A8_UINT:      return VK_FORMAT_R8G8B8A8_UINT;
        case VkmFormat::R8G8B8A8_SNORM:     return VK_FORMAT_R8G8B8A8_SNORM;
        case VkmFormat::R8G8B8A8_SINT:      return VK_FORMAT_R8G8B8A8_SINT;
        case VkmFormat::R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
        case VkmFormat::R16G16B16A16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case VkmFormat::R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case VkmFormat::D32_SFLOAT:         return VK_FORMAT_D32_SFLOAT;
        case VkmFormat::D24_UNORM_S8_UINT:  return VK_FORMAT_D24_UNORM_S8_UINT;
        case VkmFormat::D32_SFLOAT_S8_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case VkmFormat::BGRA8_UNORM:        return VK_FORMAT_B8G8R8A8_UNORM;
        default: VKM_ASSERT(false, "Unsupported texture format for Vulkan backend"); return VK_FORMAT_UNDEFINED;
    }
}

VkmFormat fromVkFormat(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_R8G8B8A8_UNORM:      return VkmFormat::R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:       return VkmFormat::R8G8B8A8_SRGB;
        case VK_FORMAT_R8G8B8A8_UINT:       return VkmFormat::R8G8B8A8_UINT;
        case VK_FORMAT_R8G8B8A8_SNORM:      return VkmFormat::R8G8B8A8_SNORM;
        case VK_FORMAT_R8G8B8A8_SINT:       return VkmFormat::R8G8B8A8_SINT;
        case VK_FORMAT_R16G16B16A16_UNORM:  return VkmFormat::R16G16B16A16_UNORM;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return VkmFormat::R16G16B16A16_SFLOAT;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return VkmFormat::R32G32B32A32_SFLOAT;
        case VK_FORMAT_D32_SFLOAT:          return VkmFormat::D32_SFLOAT;
        case VK_FORMAT_D24_UNORM_S8_UINT:   return VkmFormat::D24_UNORM_S8_UINT;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:  return VkmFormat::D32_SFLOAT_S8_UINT;
        case VK_FORMAT_B8G8R8A8_UNORM:      return VkmFormat::BGRA8_UNORM;
        default: VKM_ASSERT(false, "Unsupported VkFormat for Vulkan backend"); return VkmFormat::Undefined;
    }
}
}