// Copyright (c) 2025 Snowapril

#pragma once

#include <cstdint>

#if defined(VKM_USE_VULKAN_API) || defined(VKM_USE_METAL_API)
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif // defined(VKM_USE_VULKAN_API) || defined(VKM_USE_METAL_API)