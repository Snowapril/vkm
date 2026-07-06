// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/driver.h>
#include <volk.h>

namespace vkm
{
bool vkCheckResult(int result, const char* msg);

VkFormat toVkFormat(VkmFormat format);
VkmFormat fromVkFormat(VkFormat format);

#define VKM_VK_ASSERT(result, msg) VKM_ASSERT(vkCheckResult(result, msg), msg)
#define VKM_VK_CHECK_RESULT_MSG(result, msg) vkCheckResult(result, msg)
#define VKM_VK_CHECK_RESULT_MSG_RETURN(result, msg) if (!vkCheckResult(result, msg)) return VkmInitResult{VkmInitResultCode::Failed, msg}
}