// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/buffer_view.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

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

    VkmBuffer* VkmBufferView::resolveParent() const
    {
        VkmBuffer* parent = _driver->getRenderResourcePool()->getResource<VkmBuffer>(_bufferViewInfo._buffer);
        if (parent == nullptr)
        {
            VKM_DEBUG_ERROR("VkmBufferViewInfo::_buffer does not resolve to a live buffer");
        }
        return parent;
    }

    bool VkmBufferView::isParentAlive() const
    {
        return _driver->getRenderResourcePool()->getResource<VkmBuffer>(_bufferViewInfo._buffer) != nullptr;
    }

    VkmBuffer* VkmBufferView::tryGetParent() const
    {
        return _driver->getRenderResourcePool()->getResource<VkmBuffer>(_bufferViewInfo._buffer);
    }
} // namespace vkm
