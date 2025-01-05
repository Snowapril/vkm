// Copyright (c) 2025 Snowapril

#include "vkm/renderer/backend/common/render_resource.h"

namespace vkm
{
    VkmRenderResource::VkmRenderResource(VkmDriverBase* driver)
        : _driver(driver)
    {
    }

    VkmRenderResource::~VkmRenderResource()
    {
    }
} // namespace vkm