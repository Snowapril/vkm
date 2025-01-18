// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/swapchain.h>

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

    VkmSwapChainBase* VkmDriverBase::newSwapChain()
    {
        if (_commandQueues[(uint8_t)VkmCommandQueueType::Graphics].empty())
        {
            VKM_DEBUG_ERROR("Graphics command queue is not created. It must be created before creating swapchain");
            return nullptr;
        }

        VkmSwapChainBase* swapChain = newSwapChainInner();
        // TODO(snowapril) : pick appropriate queue instead of first one hard coded
        swapChain->setPresentQueue(_commandQueues[(uint8_t)VkmCommandQueueType::Graphics][0]);
        return swapChain;
    }
}