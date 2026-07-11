// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/texture_view.h>
#include <vkm/renderer/backend/common/driver.h>

namespace vkm
{
    VkmTexture::VkmTexture(VkmDriverBase* driver)
        : VkmRenderResource(driver)
    {
    }

    VkmTexture::~VkmTexture()
    {
    }

    bool VkmTexture::initializeTextureCommon(VkmResourceHandle handle, const VkmTextureInfo& info)
    {
        if (!initializeCommon(handle))
        {
            return false;
        }

        _textureInfo = info;
        return true;
    }

    VkmTextureView* VkmTexture::createView(VkmTextureViewInfo info)
    {
        info._texture = getHandle();
        VkmTextureView* view = _driver->newTextureView(info);
        if (view != nullptr)
        {
            _ownedViewHandles.push_back(view->getHandle());
        }
        return view;
    }
} // namespace vkm