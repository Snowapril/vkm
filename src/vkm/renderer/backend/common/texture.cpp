// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/texture_view.h>
#include <vkm/renderer/backend/common/driver.h>

#include <algorithm>

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

        // Checked here rather than per backend: Metal deliberately decouples its arrayLength
        // from _numArrayLayers for cubes, so the two backends would otherwise fail this
        // differently (or not at all) when a caller gets it wrong.
        if (info._type == VkmTextureType::Cube)
        {
            VKM_ASSERT(info._numArrayLayers == kVkmCubeFaceCount, "A cube texture must have exactly 6 array layers");
            VKM_ASSERT(info._extent.x == info._extent.y, "A cube texture must have square faces");
        }

        _textureInfo = info;
        // "No tail" until a backend that actually pages this texture reports where one starts.
        _mipTailFirstLevel = std::max(1u, info._numMipLevels);
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