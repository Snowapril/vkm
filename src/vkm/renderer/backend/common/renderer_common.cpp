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
} // namespace vkm