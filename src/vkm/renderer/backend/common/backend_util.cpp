// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/backend_util.h>

namespace vkm
{
    uint32_t operator|(VkmResourceCreateInfo lhs, VkmResourceCreateInfo rhs)
    {
        return static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs);
    };

    uint32_t operator&(VkmResourceCreateInfo lhs, VkmResourceCreateInfo rhs)
    {
        return static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs);
    };
} // namespace vkm