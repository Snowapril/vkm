// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/texture_view.h>

namespace vkm
{
    VkmTextureView::VkmTextureView(VkmDriverBase* driver)
        : VkmRenderResource(driver)
    {
    }

    VkmTextureView::~VkmTextureView()
    {
    }

    bool VkmTextureView::initializeTextureViewCommon(VkmResourceHandle handle, const VkmTextureViewInfo& info)
    {
        if (!initializeCommon(handle))
        {
            return false;
        }

        _textureViewInfo = info;
        return true;
    }
} // namespace vkm
