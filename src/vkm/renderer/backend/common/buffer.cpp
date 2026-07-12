// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/buffer_view.h>
#include <vkm/renderer/backend/common/driver.h>

namespace vkm
{
    VkmBuffer::VkmBuffer(VkmDriverBase* driver)
        : VkmRenderResource(driver)
    {
    }

    VkmBuffer::~VkmBuffer()
    {
    }

    bool VkmBuffer::initializeBufferCommon(VkmResourceHandle handle, const VkmBufferInfo& info)
    {
        if (!initializeCommon(handle))
        {
            return false;
        }

        _bufferInfo = info;
        return true;
    }

    VkmBufferView* VkmBuffer::createView(VkmBufferViewInfo info)
    {
        info._buffer = getHandle();
        VkmBufferView* view = _driver->newBufferView(info);
        if (view != nullptr)
        {
            _ownedViewHandles.push_back(view->getHandle());
        }
        return view;
    }
} // namespace vkm
