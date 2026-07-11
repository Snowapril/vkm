// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/buffer_view.h>

namespace vkm
{
    VkmBufferView::VkmBufferView(VkmDriverBase* driver)
        : VkmRenderResource(driver)
    {
    }

    VkmBufferView::~VkmBufferView()
    {
    }

    bool VkmBufferView::initializeBufferViewCommon(VkmResourceHandle handle, const VkmBufferViewInfo& info)
    {
        if (!initializeCommon(handle))
        {
            return false;
        }

        _bufferViewInfo = info;
        return true;
    }
} // namespace vkm
