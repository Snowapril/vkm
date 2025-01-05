// Copyright (c) 2024 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vulkan/vk_enum_string_helper.h>

namespace vkm
{
bool vkCheckResult(int result, const char* msg)
{
    if (result != VK_SUCCESS)
    {
        const std::string msgString = fmt::format("%s: %s", string_VkResult(static_cast<VkResult>(result)), msg);
        VKM_DEBUG_ERROR(msgString.c_str());
        return false;
    }
    return true;
}
}