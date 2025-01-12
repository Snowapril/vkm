// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/texture.h>

namespace vkm
{
    VkmDriverBase::VkmDriverBase()
    {
    }

    VkmDriverBase::~VkmDriverBase()
    {
    }

    bool VkmDriverBase::initialize(const VkmEngineLaunchOptions* options)
    {
        return initializeInner(options);
    }

    void VkmDriverBase::destroy()
    {
        destroyInner();
    }

    VkmTexture* VkmDriverBase::newTexture(const VkmTextureInfo& info)
    {
        VkmTexture* texture = newTextureInner();
        if (texture->initialize(info) == false)
        {
            delete texture;
            return nullptr;
        }

        return texture;
    }
}