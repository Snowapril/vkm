// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/texture_view.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

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

    VkmTexture* VkmTextureView::resolveParent() const
    {
        VkmTexture* parent = _driver->getRenderResourcePool()->getResource<VkmTexture>(_textureViewInfo._texture);
        if (parent == nullptr)
        {
            VKM_DEBUG_ERROR("VkmTextureViewInfo::_texture does not resolve to a live texture");
        }
        return parent;
    }

    bool VkmTextureView::isParentAlive() const
    {
        return _driver->getRenderResourcePool()->getResource<VkmTexture>(_textureViewInfo._texture) != nullptr;
    }

    VkmTexture* VkmTextureView::tryGetParent() const
    {
        return _driver->getRenderResourcePool()->getResource<VkmTexture>(_textureViewInfo._texture);
    }
} // namespace vkm
