// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/aliased_memory_heap.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/backend/common/texture_view.h>
#include <vkm/renderer/backend/common/buffer_view.h>
#include <vkm/renderer/backend/common/acceleration_structure.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/gpu_crash_handler.h>
#include <vkm/renderer/backend/common/gpu_profiler.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/upscaler.h>

#include <cstring>

namespace vkm
{
    namespace
    {
        /*
        * Vulkan (VUID-VkImageCreateInfo-usage-00963/00966) and Metal's memoryless contract
        * agree: a tile-memory-only resource may carry attachment usage and nothing else, and
        * must carry at least one. Enforced once here so no backend can hand an illegal
        * descriptor to a validation layer, and downgraded rather than rejected -- the same way
        * VkmMemoryPlacementHint::ForcePooled falls back instead of failing.
        */
        // Spelled through uint32_t rather than operator|: that operator is defined out-of-line
        // in renderer_common.cpp and so cannot seed a constexpr.
        constexpr VkmResourceCreateInfo kTransientIncompatibleFlags = (VkmResourceCreateInfo)(
            (uint32_t)VkmResourceCreateInfo::AllowTransferSrc | (uint32_t)VkmResourceCreateInfo::AllowTransferDst |
            (uint32_t)VkmResourceCreateInfo::AllowShaderRead | (uint32_t)VkmResourceCreateInfo::AllowShaderWrite |
            (uint32_t)VkmResourceCreateInfo::AllowPresent | (uint32_t)VkmResourceCreateInfo::ExternalHandleOwner |
            (uint32_t)VkmResourceCreateInfo::DeferredCreation);

        constexpr VkmResourceCreateInfo kTransientAttachmentFlags = (VkmResourceCreateInfo)(
            (uint32_t)VkmResourceCreateInfo::AllowColorAttachment |
            (uint32_t)VkmResourceCreateInfo::AllowDepthStencilAttachment);

        /*
        * An aliased texture shares its bytes with another, so anything that implies memory it
        * does not own outright, or that the render graph cannot see the lifetime of, rules it
        * out. Unlike Transient it may still be sampled and blitted -- the graph's declaration
        * is what bounds the lifetime, not the usage.
        */
        constexpr VkmResourceCreateInfo kAliasableIncompatibleFlags = (VkmResourceCreateInfo)(
            (uint32_t)VkmResourceCreateInfo::Transient | (uint32_t)VkmResourceCreateInfo::AllowPresent |
            (uint32_t)VkmResourceCreateInfo::ExternalHandleOwner |
            (uint32_t)VkmResourceCreateInfo::DeferredCreation);

        // VkmResourceCreateInfo has no operator~ or operator&=, and one local helper is less
        // surface than adding them for a single call site each.
        VkmResourceCreateInfo clearFlag(VkmResourceCreateInfo flags, VkmResourceCreateInfo toClear)
        {
            return (VkmResourceCreateInfo)((uint32_t)flags & ~(uint32_t)toClear);
        }

        VkmResourceCreateInfo clearTransient(VkmResourceCreateInfo flags)
        {
            return clearFlag(flags, VkmResourceCreateInfo::Transient);
        }

        VkmResourceCreateInfo sanitizeTransientTextureFlags(VkmResourceCreateInfo flags, const char* debugName)
        {
            if ((flags & VkmResourceCreateInfo::Transient) == 0)
            {
                return flags;
            }

            const char* name = debugName != nullptr ? debugName : "<unnamed>";
            if ((flags & kTransientIncompatibleFlags) != 0)
            {
                VKM_DEBUG_WARN(fmt::format("VkmResourceCreateInfo::Transient on texture '{}' is combined with a "
                                           "non-attachment usage; the texture will be device-backed", name).c_str());
                return clearTransient(flags);
            }
            // Such a texture carries no image usage at all and vkCreateImage rejects it on that
            // ground alone; clearing the bit here only keeps it from failing on the more
            // confusing TRANSIENT_ATTACHMENT VUID instead.
            if ((flags & kTransientAttachmentFlags) == 0)
            {
                VKM_DEBUG_WARN(fmt::format("VkmResourceCreateInfo::Transient on texture '{}' carries no attachment "
                                           "usage; the texture will be device-backed", name).c_str());
                return clearTransient(flags);
            }
            return flags;
        }

        VkmResourceCreateInfo sanitizeTransientBufferFlags(VkmResourceCreateInfo flags, const char* debugName)
        {
            if ((flags & VkmResourceCreateInfo::Transient) == 0)
            {
                return flags;
            }

            // Neither Vulkan nor Metal has a transient buffer; dropped loudly rather than
            // silently, so a discarded memory request never costs a bisect.
            VKM_DEBUG_WARN(fmt::format("VkmResourceCreateInfo::Transient is texture-only; ignored on buffer '{}'",
                                       debugName != nullptr ? debugName : "<unnamed>").c_str());
            return clearTransient(flags);
        }

        VkmResourceCreateInfo sanitizeAliasableTextureFlags(VkmResourceCreateInfo flags, const char* debugName,
                                                           bool backendSupportsAliasing)
        {
            if ((flags & VkmResourceCreateInfo::Aliasable) == 0)
            {
                return flags;
            }

            const char* name = debugName != nullptr ? debugName : "<unnamed>";
            if (!backendSupportsAliasing)
            {
                // Accepted and downgraded rather than rejected, so one VkmTextureInfo stays
                // portable across every backend -- the contract Transient and ForcePooled carry.
                VKM_DEBUG_WARN(fmt::format("VkmResourceCreateInfo::Aliasable is not supported by this backend; "
                                           "texture '{}' will own its memory", name).c_str());
                return clearFlag(flags, VkmResourceCreateInfo::Aliasable);
            }
            if ((flags & kAliasableIncompatibleFlags) != 0)
            {
                VKM_DEBUG_WARN(fmt::format("VkmResourceCreateInfo::Aliasable on texture '{}' is combined with a flag "
                                           "that implies memory it does not own; the texture will own its memory",
                                           name).c_str());
                return clearFlag(flags, VkmResourceCreateInfo::Aliasable);
            }
            // Attachment use is the only use VkmRenderGraph::compile() can check for an omitted
            // declaration, and the only place the mandatory discard has any meaning.
            if ((flags & kTransientAttachmentFlags) == 0)
            {
                VKM_DEBUG_WARN(fmt::format("VkmResourceCreateInfo::Aliasable on texture '{}' carries no attachment "
                                           "usage; the texture will own its memory", name).c_str());
                return clearFlag(flags, VkmResourceCreateInfo::Aliasable);
            }
            return flags;
        }

        VkmResourceCreateInfo sanitizeAliasableBufferFlags(VkmResourceCreateInfo flags, const char* debugName)
        {
            if ((flags & VkmResourceCreateInfo::Aliasable) == 0)
            {
                return flags;
            }

            // A buffer's use is invisible to the render graph -- it is never an attachment, so
            // the undeclared-use check that makes aliasing safe cannot exist for one. Deliberately
            // out of scope rather than silently unsafe.
            VKM_DEBUG_WARN(fmt::format("VkmResourceCreateInfo::Aliasable is texture-only; ignored on buffer '{}'",
                                       debugName != nullptr ? debugName : "<unnamed>").c_str());
            return clearFlag(flags, VkmResourceCreateInfo::Aliasable);
        }
    }

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
        _gpuProfiler = std::make_unique<VkmGpuProfiler>(this);

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

        // After initializeInner(), which is what decides whether this device can alias at all.
        if (supportsResourceAliasing())
        {
            _aliasedMemoryHeap = std::make_unique<VkmAliasedMemoryHeap>(this);
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

        // After the device and the command queues exist, since the timestamp pool is a device
        // object and the profiler labels its timelines by queue.
        if (_gpuProfiler->initialize() == false)
        {
            return VkmInitResult{VkmInitResultCode::Failed, "Failed to initialize GPU profiler"};
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

        // Before destroyInner(), which tears down the device the timestamp pool belongs to.
        if (_gpuProfiler)
        {
            _gpuProfiler->destroy();
        }

        // Before destroyInner() too: the blocks are backend allocations owned by the device it
        // is about to tear down.
        if (_aliasedMemoryHeap)
        {
            _aliasedMemoryHeap->destroy();
        }

        destroyInner();
    }

    VkmTexture* VkmDriverBase::newTexture(const VkmTextureInfo& textureInfo)
    {
        // Sanitized before anything else sees it, so the backends, getTextureInfo() and the
        // render-pass guard all read the same, legal flag set.
        VkmTextureInfo info = textureInfo;
        info._flags = sanitizeTransientTextureFlags(info._flags, info._debugName);
        // After the Transient pass, so a texture asking for both is told about the one that
        // survived rather than about a combination it no longer has.
        info._flags = sanitizeAliasableTextureFlags(info._flags, info._debugName, supportsResourceAliasing());

        VkmResourcePoolType poolType = VkmResourcePoolType::Default;
        if ((info._flags & VkmResourceCreateInfo::Transient) != 0)
        {
            poolType = VkmResourcePoolType::Transient;
        }
        else if ((info._flags & VkmResourceCreateInfo::Aliasable) != 0)
        {
            poolType = VkmResourcePoolType::Aliased;
        }

        VkmTexture* texture = newTextureInner();
        VkmResourceHandle handle = _renderResourcePool->allocateTexture(texture, poolType);
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
        // Distinguishes "0 bytes because the allocation is tile-only" and "0 bytes because the
        // bytes are shared and counted once as a heap block" from "0 bytes because nothing
        // reported a size" in the memory report.
        tag.metadata = texture->isTransient() ? "transient" : (texture->isAliasable() ? "aliased" : "");
        tag.type = texture->getResourceType();
        _renderResourcePool->tagResource(handle, tag);
        _renderResourcePool->onResourceInitialized(handle);

        if (_debugNamingEnabled && info._debugName != nullptr)
        {
            texture->setDebugName(info._debugName);
        }

        return texture;
    }

    VkmBuffer* VkmDriverBase::newBuffer(const VkmBufferInfo& bufferInfo)
    {
        VkmBufferInfo info = bufferInfo;
        info._flags = sanitizeTransientBufferFlags(info._flags, info._debugName);
        info._flags = sanitizeAliasableBufferFlags(info._flags, info._debugName);

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

    VkmStagingBuffer* VkmDriverBase::newStagingBuffer(const VkmStagingBufferInfo& stagingBufferInfo)
    {
        VkmStagingBufferInfo info = stagingBufferInfo;
        info._flags = sanitizeTransientBufferFlags(info._flags, info._debugName);
        info._flags = sanitizeAliasableBufferFlags(info._flags, info._debugName);

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

    bool VkmDriverBase::uploadToBuffer(VkmResourceHandle dstBuffer, const void* data, uint64_t size, uint64_t dstOffset, VkmResourceUploadMode mode)
    {
        VkmBuffer* buffer = _renderResourcePool->getResource<VkmBuffer>(dstBuffer);
        if (buffer == nullptr)
        {
            VKM_DEBUG_ERROR("uploadToBuffer: invalid buffer handle");
            return false;
        }
        if (dstOffset + size > buffer->getBufferInfo()._size)
        {
            VKM_DEBUG_ERROR("uploadToBuffer: the destination range is outside the buffer");
            return false;
        }

        // The destination's own memory decides what is possible; the mode only chooses among
        // what that already made available.
        const bool hostCopyAvailable = buffer->isHostWritable();
        if (mode == VkmResourceUploadMode::ForceHostCopy && !hostCopyAvailable)
        {
            VKM_DEBUG_WARN("uploadToBuffer: ForceHostCopy requested but this buffer's memory is not host-writable; using the staging path");
        }
        if (hostCopyAvailable && mode != VkmResourceUploadMode::ForceStaging)
        {
            // No staging buffer, no command buffer, no submit, no wait -- the CPU writes the
            // buffer's memory in place.
            void* mapped = buffer->map();
            if (mapped != nullptr)
            {
                std::memcpy(static_cast<uint8_t*>(mapped) + dstOffset, data, size);
                buffer->unmap();
                return true;
            }
            // isHostWritable() said yes but the mapping is gone; the staging path below writes
            // the same bytes, so fall through rather than fail.
            VKM_DEBUG_WARN("uploadToBuffer: a host-writable buffer returned no mapped pointer; using the staging path");
        }

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
        if (mapped == nullptr)
        {
            VKM_DEBUG_ERROR("uploadToBuffer: failed to map the staging buffer");
            _renderResourcePool->releaseResource(stagingBuffer->getHandle());
            return false;
        }
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
        commandQueue->getCommandBufferPool()->release(commandBuffer);

        _renderResourcePool->releaseResource(stagingBuffer->getHandle());
        return true;
    }

    bool VkmDriverBase::uploadToTexture(VkmResourceHandle dstTexture, const void* data, uint64_t size,
                                        uint32_t mipLevel, uint32_t arrayLayer, VkmResourceUploadMode mode)
    {
        if ((getDriverCapabilityFlags() & VkmDriverCapabilityFlags::TextureUpload) == 0)
        {
            VKM_DEBUG_ERROR("uploadToTexture: this backend does not implement texture upload");
            return false;
        }

        VkmTexture* texture = _renderResourcePool->getResource<VkmTexture>(dstTexture);
        if (texture == nullptr)
        {
            VKM_DEBUG_ERROR("uploadToTexture: invalid texture handle");
            return false;
        }

        // The destination's own memory decides what is possible; the mode only chooses among
        // what that already made available.
        const bool hostCopyAvailable = texture->isHostWritable();
        if (mode == VkmResourceUploadMode::ForceHostCopy && !hostCopyAvailable)
        {
            VKM_DEBUG_WARN("uploadToTexture: ForceHostCopy requested but this texture's memory is not host-writable; using the staging path");
        }
        if (hostCopyAvailable && mode != VkmResourceUploadMode::ForceStaging)
        {
            // No staging buffer, no command buffer, no submit, no wait -- the CPU writes the
            // texture's memory in place.
            return texture->writeRegion(data, size, mipLevel, arrayLayer);
        }

        VkmStagingBufferInfo stagingInfo{};
        stagingInfo._flags = VkmResourceCreateInfo::AllowTransferSrc;
        stagingInfo._size = size;
        stagingInfo._debugName = "UploadToTextureStaging";
        VkmStagingBuffer* stagingBuffer = newStagingBuffer(stagingInfo);
        if (stagingBuffer == nullptr)
        {
            VKM_DEBUG_ERROR("uploadToTexture: failed to create staging buffer");
            return false;
        }

        void* mapped = stagingBuffer->map();
        std::memcpy(mapped, data, size);
        stagingBuffer->unmap();
        stagingBuffer->flush(0, size);

        VkmCommandQueueBase* commandQueue = getCommandQueue(VkmCommandQueueType::Graphics, 0);
        VkmCommandBufferBase* commandBuffer = commandQueue->getCommandBufferPool()->allocate();
        commandBuffer->beginCommandBuffer();
        commandBuffer->copyBufferToTexture(stagingBuffer->getHandle(), dstTexture, 0, mipLevel, arrayLayer);
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

    VkmTextureReadbackResult VkmDriverBase::readbackTexture(VkmResourceHandle textureHandle, uint32_t arrayLayer)
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
        commandBuffer->copyTextureToBuffer(textureHandle, stagingBuffer->getHandle(), 0, arrayLayer);
        commandBuffer->endCommandBuffer();

        CommandSubmitInfo submitInfo;
        submitInfo.commandBuffers[0] = commandBuffer;
        submitInfo.commandBufferCount = 1;
        VkmGpuEventTimelineObject submitResult = commandQueue->submit(submitInfo);
        if (submitResult._gpuEventTimeline != nullptr)
        {
            submitResult._gpuEventTimeline->waitIdle(MAX_GPU_TIMEOUT_PER_FRAME);
        }
        commandQueue->getCommandBufferPool()->release(commandBuffer);

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

    VkmAccelerationStructure* VkmDriverBase::newAccelerationStructure(const VkmAccelerationStructureInfo& info)
    {
        // Checked here rather than in each backend so the message is the same everywhere and a
        // backend's Inner() never has to guess why it was called.
        if ((_driverCapabilityFlags & VkmDriverCapabilityFlags::RayTracing) == 0)
        {
            VKM_DEBUG_ERROR("newAccelerationStructure requires VkmDriverCapabilityFlags::RayTracing");
            return nullptr;
        }

        VkmAccelerationStructure* accelerationStructure = newAccelerationStructureInner();
        if (accelerationStructure == nullptr)
        {
            return nullptr;
        }
        VkmResourceHandle handle =
            _renderResourcePool->allocateAccelerationStructure(accelerationStructure, VkmResourcePoolType::Default);
        if (accelerationStructure->initialize(handle, info) == false)
        {
            VKM_DEBUG_ERROR("Failed to initialize acceleration structure");
            if (handle.isValid())
                _renderResourcePool->releaseResource(handle);
            else
                delete accelerationStructure;
            return nullptr;
        }

        VkmResourceMemoryTag tag{};
        tag.requestedSize = 0;
        tag.allocatedSize = accelerationStructure->getAllocatedSize();
        tag.alignment = accelerationStructure->getMemoryAlignment();
        tag.name = info._debugName != nullptr ? info._debugName : "";
        tag.type = accelerationStructure->getResourceType();
        _renderResourcePool->tagResource(handle, tag);
        _renderResourcePool->onResourceInitialized(handle);

        if (_debugNamingEnabled && info._debugName != nullptr)
        {
            accelerationStructure->setDebugName(info._debugName);
        }
        return accelerationStructure;
    }

    VkmUpscalerBase* VkmDriverBase::newUpscaler(const VkmUpscalerDescriptor& descriptor)
    {
        // Checked here rather than in each backend so the message is the same everywhere and a
        // backend's Inner() never has to guess why it was called.
        if ((_driverCapabilityFlags & VkmDriverCapabilityFlags::TemporalUpscaling) == 0)
        {
            VKM_DEBUG_ERROR("newUpscaler requires VkmDriverCapabilityFlags::TemporalUpscaling");
            return nullptr;
        }

        VkmUpscalerBase* upscaler = newUpscalerInner();
        if (upscaler == nullptr)
        {
            return nullptr;
        }
        if (!upscaler->initialize(this, descriptor))
        {
            delete upscaler;
            return nullptr;
        }
        return upscaler;
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

    bool VkmDriverBase::resolveSwapChainFormats(VkmPipelineStateDescriptor& desc, std::string* outError) const
    {
        for (VkmColorBlendAttachmentState& attachment : desc.colorAttachments)
        {
            if (attachment.format == VkmFormat::Swapchain)
            {
                if (_swapChainColorFormat == VkmFormat::Undefined || _swapChainColorFormat == VkmFormat::Swapchain)
                {
                    if (outError != nullptr)
                    {
                        *outError = "Pipeline requests \"swapchain\" color format but no swapchain color format has been resolved";
                    }
                    return false;
                }
                attachment.format = _swapChainColorFormat;
            }
        }
        return true;
    }

    void VkmDriverBase::waitIdle(const uint64_t timeoutMs)
    {
        for (const std::vector<VkmCommandQueueBase*>& queuesOfType : _commandQueues)
        {
            for (VkmCommandQueueBase* commandQueue : queuesOfType)
            {
                commandQueue->waitIdle(timeoutMs);
            }
        }
    }

    VkmPipelineStateBase* VkmDriverBase::newPipelineState(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError)
    {
        VkmPipelineStateDescriptor resolvedDesc = desc;
        if (!resolveSwapChainFormats(resolvedDesc, outError))
        {
            return nullptr;
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

    VkmResourceTableBase* VkmDriverBase::newResourceTable(
        const VkmPipelineStateBase* pipelineState,
        VkmResourceSetKind kind,
        const std::vector<VkmTableResourceEntry>& entries,
        std::string* outError)
    {
        VkmResourceTableBase* table = newResourceTableInner();
        if (table == nullptr)
        {
            return nullptr;
        }
        if (!table->initialize(pipelineState, kind, entries, outError))
        {
            delete table;
            return nullptr;
        }
        return table;
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