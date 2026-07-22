// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/backend/common/texture_view.h>
#include <vkm/renderer/backend/common/buffer_view.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/gpu_crash_handler.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>

#include <cstring>

namespace vkm
{
    VkmDriverBase::VkmDriverBase()
    {
    }

    VkmDriverBase::~VkmDriverBase()
    {
    }

    VkmInitResult VkmDriverBase::initialize(const VkmEngineLaunchOptions* options)
    {
        _renderResourcePool.reset(newRenderResourcePoolInner());
        _deferredReclaimer = std::make_unique<VkmDeferredResourceReclaimer>(this);
        _gpuCrashHandler = std::make_unique<VkmGpuCrashHandler>(this);

        if (options != nullptr)
        {
            _launchOptions = *options;
            _debugNamingEnabled = options->enableValidationLayer || options->enableGpuCapture;
            _gpuCrashDumpEnabled = options->enableGpuCrashDump;
        }
        else
        {
            _launchOptions = DEFAULT_ENGINE_LAUNCH_OPTIONS;
            _debugNamingEnabled = false;
            _gpuCrashDumpEnabled = false;
        }

        VkmInitResult result = initializeInner(options);
        if (result.code != VkmInitResultCode::Success)
        {
            return result;
        }

        // Decide the swapchain color format now (after the device is valid) so it is known
        // before any pipeline state is created -- pipelines loaded during postDriverReady
        // resolve a "swapchain" color format against this, and addSwapChain later creates the
        // swapchain with the same format.
        _swapChainColorFormat = selectSwapChainColorFormat(_launchOptions.enableHdr);

        if (_renderResourcePool->initialize() == false)
        {
            return VkmInitResult{VkmInitResultCode::Failed, "Failed to initialize render resource pool"};
        }

        if (setUpPredefinedCommandQueues() == false)
        {
            return VkmInitResult{VkmInitResultCode::Failed, "Failed to set up predefined command queues"};
        }

        if (postInitializeInner() == false)
        {
            return VkmInitResult{VkmInitResultCode::Failed, "Failed post-initialization"};
        }

        _deferredReclaimer->start();

        return VkmInitResult{VkmInitResultCode::Success, ""};
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
        // Stop and drain the reclaimer first -- destroyInner() tears down driver-owned
        // objects (VmaAllocator, etc.) that pending entries' resource destructors may need.
        if (_deferredReclaimer)
        {
            _deferredReclaimer->stop();
        }

        destroyInner();
    }

    VkmTexture* VkmDriverBase::newTexture(const VkmTextureInfo& info)
    {
        VkmTexture* texture = newTextureInner();
        VkmResourceHandle handle = _renderResourcePool->allocateTexture(texture, VkmResourcePoolType::Default);
        if (texture->initialize(handle, info) == false)
        {
            VKM_DEBUG_ERROR("Failed to initialize texture");
            if (handle.isValid())
                _renderResourcePool->releaseResource(handle);
            else
                delete texture;
            return nullptr;
        }

        VkmResourceMemoryTag tag{};
        tag.requestedSize = computeTextureByteSize(info);
        tag.allocatedSize = texture->getAllocatedSize();
        tag.alignment = texture->getMemoryAlignment();
        tag.name = info._debugName != nullptr ? info._debugName : "";
        tag.type = texture->getResourceType();
        _renderResourcePool->tagResource(handle, tag);
        _renderResourcePool->onResourceInitialized(handle);

        if (_debugNamingEnabled && info._debugName != nullptr)
        {
            texture->setDebugName(info._debugName);
        }

        return texture;
    }

    VkmBuffer* VkmDriverBase::newBuffer(const VkmBufferInfo& info)
    {
        VkmBuffer* buffer = newBufferInner();
        VkmResourceHandle handle = _renderResourcePool->allocateBuffer(buffer, VkmResourcePoolType::Default);
        if (buffer->initialize(handle, info) == false)
        {
            VKM_DEBUG_ERROR("Failed to initialize buffer");
            if (handle.isValid())
                _renderResourcePool->releaseResource(handle);
            else
                delete buffer;
            return nullptr;
        }

        VkmResourceMemoryTag tag{};
        tag.requestedSize = info._size;
        tag.allocatedSize = buffer->getAllocatedSize();
        tag.alignment = buffer->getMemoryAlignment();
        tag.name = info._debugName != nullptr ? info._debugName : "";
        tag.type = buffer->getResourceType();
        _renderResourcePool->tagResource(handle, tag);
        _renderResourcePool->onResourceInitialized(handle);

        if (_debugNamingEnabled && info._debugName != nullptr)
        {
            buffer->setDebugName(info._debugName);
        }

        return buffer;
    }

    VkmStagingBuffer* VkmDriverBase::newStagingBuffer(const VkmStagingBufferInfo& info)
    {
        VkmStagingBuffer* stagingBuffer = newStagingBufferInner();
        VkmResourceHandle handle = _renderResourcePool->allocateStagingBuffer(stagingBuffer, VkmResourcePoolType::Default);
        if (stagingBuffer->initialize(handle, info) == false)
        {
            VKM_DEBUG_ERROR("Failed to initialize staging buffer");
            if (handle.isValid())
                _renderResourcePool->releaseResource(handle);
            else
                delete stagingBuffer;
            return nullptr;
        }

        VkmResourceMemoryTag tag{};
        tag.requestedSize = info._size;
        tag.allocatedSize = stagingBuffer->getAllocatedSize();
        tag.alignment = stagingBuffer->getMemoryAlignment();
        tag.name = info._debugName != nullptr ? info._debugName : "";
        tag.type = stagingBuffer->getResourceType();
        _renderResourcePool->tagResource(handle, tag);
        _renderResourcePool->onResourceInitialized(handle);

        if (_debugNamingEnabled && info._debugName != nullptr)
        {
            stagingBuffer->setDebugName(info._debugName);
        }

        return stagingBuffer;
    }

    bool VkmDriverBase::uploadToBuffer(VkmResourceHandle dstBuffer, const void* data, uint64_t size, uint64_t dstOffset)
    {
        VkmStagingBufferInfo stagingInfo{};
        stagingInfo._flags = VkmResourceCreateInfo::AllowTransferSrc;
        stagingInfo._size = size;
        stagingInfo._debugName = "UploadToBufferStaging";
        VkmStagingBuffer* stagingBuffer = newStagingBuffer(stagingInfo);
        if (stagingBuffer == nullptr)
        {
            VKM_DEBUG_ERROR("uploadToBuffer: failed to create staging buffer");
            return false;
        }

        void* mapped = stagingBuffer->map();
        std::memcpy(mapped, data, size);
        stagingBuffer->unmap();
        stagingBuffer->flush(0, size);

        VkmCommandQueueBase* commandQueue = getCommandQueue(VkmCommandQueueType::Graphics, 0);
        VkmCommandBufferBase* commandBuffer = commandQueue->getCommandBufferPool()->allocate();
        commandBuffer->beginCommandBuffer();
        commandBuffer->copyBuffer(stagingBuffer->getHandle(), dstBuffer, 0, dstOffset, size);
        commandBuffer->endCommandBuffer();

        CommandSubmitInfo submitInfo;
        submitInfo.commandBuffers[0] = commandBuffer;
        submitInfo.commandBufferCount = 1;
        VkmGpuEventTimelineObject submitResult = commandQueue->submit(submitInfo);
        if (submitResult._gpuEventTimeline != nullptr)
        {
            submitResult._gpuEventTimeline->waitIdle(MAX_GPU_TIMEOUT_PER_FRAME);
        }

        _renderResourcePool->releaseResource(stagingBuffer->getHandle());
        return true;
    }

    VkmTextureReadbackResult VkmDriverBase::readbackTexture(VkmResourceHandle textureHandle)
    {
        VkmTextureReadbackResult result{};

        VkmTexture* texture = _renderResourcePool->getResource<VkmTexture>(textureHandle);
        if (texture == nullptr)
        {
            VKM_DEBUG_ERROR("readbackTexture: invalid texture handle");
            return result;
        }

        const VkmTextureInfo& textureInfo = texture->getTextureInfo();
        const uint32_t bytesPerTexel = vkmBytesPerTexel(textureInfo._format);
        if (bytesPerTexel == 0)
        {
            VKM_DEBUG_ERROR("readbackTexture: unsupported texture format");
            return result;
        }

        const uint64_t byteSize =
            static_cast<uint64_t>(textureInfo._extent.x) * textureInfo._extent.y * bytesPerTexel;

        // AllowTransferDst marks this as a readback (GPU-write, CPU-read) staging buffer --
        // Vulkan selects TRANSFER_DST usage + host-readable memory, WebGPU CopyDst|MapRead.
        VkmStagingBufferInfo stagingInfo{};
        stagingInfo._flags = VkmResourceCreateInfo::AllowTransferDst;
        stagingInfo._size = byteSize;
        stagingInfo._debugName = "ReadbackTextureStaging";
        VkmStagingBuffer* stagingBuffer = newStagingBuffer(stagingInfo);
        if (stagingBuffer == nullptr)
        {
            VKM_DEBUG_ERROR("readbackTexture: failed to create staging buffer");
            return result;
        }

        VkmCommandQueueBase* commandQueue = getCommandQueue(VkmCommandQueueType::Graphics, 0);
        VkmCommandBufferBase* commandBuffer = commandQueue->getCommandBufferPool()->allocate();
        commandBuffer->beginCommandBuffer();
        commandBuffer->copyTextureToBuffer(textureHandle, stagingBuffer->getHandle(), 0);
        commandBuffer->endCommandBuffer();

        CommandSubmitInfo submitInfo;
        submitInfo.commandBuffers[0] = commandBuffer;
        submitInfo.commandBufferCount = 1;
        VkmGpuEventTimelineObject submitResult = commandQueue->submit(submitInfo);
        if (submitResult._gpuEventTimeline != nullptr)
        {
            submitResult._gpuEventTimeline->waitIdle(MAX_GPU_TIMEOUT_PER_FRAME);
        }

        stagingBuffer->invalidate(0, byteSize);
        const void* mapped = stagingBuffer->map();
        if (mapped != nullptr)
        {
            result.pixels.resize(byteSize);
            std::memcpy(result.pixels.data(), mapped, byteSize);
            result.width = textureInfo._extent.x;
            result.height = textureInfo._extent.y;
            result.channels = bytesPerTexel; // uncompressed color formats: channels == bytes for 8-bit RGBA/BGRA
        }
        else
        {
            VKM_DEBUG_ERROR("readbackTexture: failed to map staging buffer");
        }
        stagingBuffer->unmap();

        _renderResourcePool->releaseResource(stagingBuffer->getHandle());
        return result;
    }

    VkmSampler* VkmDriverBase::newSampler(const VkmSamplerInfo& info)
    {
        VkmSampler* sampler = newSamplerInner();
        VkmResourceHandle handle = _renderResourcePool->allocateSampler(sampler, VkmResourcePoolType::Default);
        if (sampler->initialize(handle, info) == false)
        {
            VKM_DEBUG_ERROR("Failed to initialize sampler");
            if (handle.isValid())
                _renderResourcePool->releaseResource(handle);
            else
                delete sampler;
            return nullptr;
        }

        VkmResourceMemoryTag tag{};
        tag.requestedSize = 0;
        tag.allocatedSize = sampler->getAllocatedSize();
        tag.alignment = sampler->getMemoryAlignment();
        tag.name = info._debugName != nullptr ? info._debugName : "";
        tag.type = sampler->getResourceType();
        _renderResourcePool->tagResource(handle, tag);
        _renderResourcePool->onResourceInitialized(handle);

        if (_debugNamingEnabled && info._debugName != nullptr)
        {
            sampler->setDebugName(info._debugName);
        }

        return sampler;
    }

    VkmTextureView* VkmDriverBase::newTextureView(const VkmTextureViewInfo& info)
    {
        VkmTextureView* textureView = newTextureViewInner();
        VkmResourceHandle handle = _renderResourcePool->allocateTextureView(textureView, VkmResourcePoolType::Default);
        if (textureView->initialize(handle, info) == false)
        {
            VKM_DEBUG_ERROR("Failed to initialize texture view");
            if (handle.isValid())
                _renderResourcePool->releaseResource(handle);
            else
                delete textureView;
            return nullptr;
        }

        VkmResourceMemoryTag tag{};
        tag.requestedSize = 0;
        tag.allocatedSize = textureView->getAllocatedSize();
        tag.alignment = textureView->getMemoryAlignment();
        tag.name = info._debugName != nullptr ? info._debugName : "";
        tag.type = textureView->getResourceType();
        _renderResourcePool->tagResource(handle, tag);
        _renderResourcePool->onResourceInitialized(handle);

        if (_debugNamingEnabled && info._debugName != nullptr)
        {
            textureView->setDebugName(info._debugName);
        }

        return textureView;
    }

    VkmBufferView* VkmDriverBase::newBufferView(const VkmBufferViewInfo& info)
    {
        VkmBufferView* bufferView = newBufferViewInner();
        VkmResourceHandle handle = _renderResourcePool->allocateBufferView(bufferView, VkmResourcePoolType::Default);
        if (bufferView->initialize(handle, info) == false)
        {
            VKM_DEBUG_ERROR("Failed to initialize buffer view");
            if (handle.isValid())
                _renderResourcePool->releaseResource(handle);
            else
                delete bufferView;
            return nullptr;
        }

        VkmResourceMemoryTag tag{};
        tag.requestedSize = 0;
        tag.allocatedSize = bufferView->getAllocatedSize();
        tag.alignment = bufferView->getMemoryAlignment();
        tag.name = info._debugName != nullptr ? info._debugName : "";
        tag.type = bufferView->getResourceType();
        _renderResourcePool->tagResource(handle, tag);
        _renderResourcePool->onResourceInitialized(handle);

        if (_debugNamingEnabled && info._debugName != nullptr)
        {
            bufferView->setDebugName(info._debugName);
        }

        return bufferView;
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

    VkmPipelineStateBase* VkmDriverBase::newPipelineState(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError)
    {
        // Resolve any "swapchain" color format sentinel to the concrete swapchain format before
        // the backend consumes it -- the format converters (getMTLPixelFormat/toVkFormat/...)
        // must never see VkmFormat::Swapchain.
        VkmPipelineStateDescriptor resolvedDesc = desc;
        for (VkmColorBlendAttachmentState& attachment : resolvedDesc.colorAttachments)
        {
            if (attachment.format == VkmFormat::Swapchain)
            {
                if (_swapChainColorFormat == VkmFormat::Undefined || _swapChainColorFormat == VkmFormat::Swapchain)
                {
                    if (outError != nullptr)
                    {
                        *outError = "Pipeline requests \"swapchain\" color format but no swapchain color format has been resolved";
                    }
                    return nullptr;
                }
                attachment.format = _swapChainColorFormat;
            }
        }

        VkmPipelineStateBase* pipelineState = newPipelineStateInner();
        if (pipelineState->initialize(resolvedDesc, shaderCacheDir, outError) == false)
        {
            VKM_DEBUG_ERROR("Failed to initialize pipeline state");
            delete pipelineState;
            return nullptr;
        }

        return pipelineState;
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