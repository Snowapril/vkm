// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#import <Metal/MTLPixelFormat.h>

namespace vkm
{
    MTLPixelFormat getMTLPixelFormat(VkmFormat format);
}
