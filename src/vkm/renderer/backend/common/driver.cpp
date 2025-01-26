// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <vkm/renderer/backend/common/command_queue.h>

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
        if (initializeInner(options) == false)
        {
            return false;
        }

        if (setUpPredefinedCommandQueues() == false)
        {
            return false;
        }

        return true;
    }

    bool VkmDriverBase::setUpPredefinedCommandQueues()
    {
        // Note(snowapril) : at now, there is no separate queue for present. graphics queue must have present capability
        _commandQueues[(uint8_t)VkmCommandQueueType::Graphics].push_back(newCommandQueue(VkmCommandQueueType::Graphics, 0, "MainGraphics"));
        _commandQueues[(uint8_t)VkmCommandQueueType::Compute].push_back(newCommandQueue(VkmCommandQueueType::Compute, 0, "AsyncCompute"));

        // TODO(snowapril) : transfer queue is not necessary for now. but it will be added in future

        return true;
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
            VKM_DEBUG_ERROR("Failed to initialize texture");
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

    VkmCommandQueueBase* VkmDriverBase::newCommandQueue(const VkmCommandQueueType queueType, const uint32_t commandQueueIndex, const char* name)
    {
        VkmCommandQueueBase* commandQueue = newCommandQueueInner();
        if (commandQueue->initialize(queueType, commandQueueIndex, name) == false)
        {
            VKM_DEBUG_ERROR("Failed to initialize command queue");
            delete commandQueue;
            return nullptr;
        }
        return commandQueue;
    }
}