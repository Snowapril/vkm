// Copyright (c) 2024 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>

namespace vkm
{
bool vkCheckResult(int result, const char* msg);

#define VKM_VK_CHECK_RESULT_MSG(result, msg) vkCheckResult(result, msg)
#define VKM_VK_CHECK_RESULT_MSG_RETURN(result, msg) if (!vkCheckResult(result, msg)) return false
}