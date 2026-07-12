// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/driver.h>
#include <volk.h>

namespace vkm
{
class VkmDriverVulkan;

bool vkCheckResult(int result, const char* msg);

/*
* @brief Registers the currently-initializing Vulkan driver so vkCheckResult() can route a
* detected VK_ERROR_DEVICE_LOST to its VkmGpuCrashHandler. Exactly one Vulkan driver is ever
* active in this engine; VkmDriverVulkan::initializeInner()/destroyInner() call this with
* itself/nullptr respectively.
*/
void setActiveVulkanDriver(VkmDriverVulkan* driver);

VkFormat toVkFormat(VkmFormat format);
VkmFormat fromVkFormat(VkFormat format);

VkImageUsageFlags toVkImageUsageFlags(VkmResourceCreateInfo flags);
VkBufferUsageFlags toVkBufferUsageFlags(VkmResourceCreateInfo flags);

VkFilter toVkFilter(VkmFilterMode filterMode);
VkSamplerMipmapMode toVkSamplerMipmapMode(VkmMipmapMode mipmapMode);
VkSamplerAddressMode toVkSamplerAddressMode(VkmAddressMode addressMode);
VkCompareOp toVkCompareOp(VkmCompareOp compareOp);

// Approximate bytes-per-texel for the committed-vs-pooled size heuristic; not exact for
// compressed formats (none are defined in VkmFormat today), only used for a size threshold.
uint32_t vkFormatBytesPerTexel(VkmFormat format);

#define VKM_VK_ASSERT(result, msg) VKM_ASSERT(vkCheckResult(result, msg), msg)
#define VKM_VK_CHECK_RESULT_MSG(result, msg) vkCheckResult(result, msg)
#define VKM_VK_CHECK_RESULT_MSG_RETURN(result, msg) if (!vkCheckResult(result, msg)) return VkmInitResult{VkmInitResultCode::Failed, msg}
}