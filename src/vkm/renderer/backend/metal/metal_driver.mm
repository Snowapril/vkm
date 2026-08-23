// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_resource_table.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_buffer.h>
#include <vkm/renderer/backend/metal/metal_staging_buffer.h>
#include <vkm/renderer/backend/metal/metal_sampler.h>
#include <vkm/renderer/backend/metal/metal_acceleration_structure.h>
#include <vkm/renderer/backend/metal/metal_texture_view.h>
#include <vkm/renderer/backend/metal/metal_buffer_view.h>
#include <vkm/renderer/backend/metal/metal_gpu_heap_allocator.h>
#include <vkm/renderer/backend/metal/metal_gpu_image_heap.h>
#include <vkm/renderer/backend/metal/metal_swapchain.h>
#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <vkm/renderer/backend/metal/metal_pipeline_state.h>
#include <vkm/renderer/backend/metal/metal_render_resource_pool.h>
#include <vkm/renderer/backend/metal/metal_upscaler.h>
#include <vkm/renderer/backend/metal/metal_util.h>

#import <Metal/MTLDevice.h>
#import <Metal/MTL4Counters.h>
#import <Metal/MTLHeap.h>
#import <MetalFX/MetalFX.h>

#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#endif

#if defined(VKM_GPU_CAPTURE)
#import <Metal/MTLCaptureManager.h>
#import <Metal/MTLCaptureScope.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#endif // VKM_GPU_CAPTURE

namespace vkm
{
    namespace
    {
        /*
        * Page size every placement-sparse texture and its tile heap are created with. Lives here
        * rather than in the header because metal_driver.h forward-declares its Metal types instead
        * of importing Metal, and this is an NS_ENUM that cannot be forward-declared.
        *
        * The smallest Metal offers, chosen for the mip tail rather than for the tile. A texture's
        * tail -- every level too small to fill one tile -- is indivisible and permanently resident,
        * so the page size sets the floor on what streaming can ever give back. Measured on an
        * M3 Pro against a 2048x2048 RGBA8 chain: 16 KiB pages put the tail at level 6 and 16 KiB,
        * 64 KiB pages at level 5 and 64 KiB, 256 KiB at level 4 and 256 KiB. The smallest page
        * therefore streams two more levels and leaves a sixteenth of the residue.
        * A texture and the heap it is placed in must agree on this, so there is one value.
        */
        constexpr MTLSparsePageSize kSparsePageSize = MTLSparsePageSize16;
    } // namespace

    VkmDriverMetal::VkmDriverMetal(id<MTLDevice> mtlDevice)
        : VkmDriverBase(), _mtlDevice(mtlDevice)
    {
        
    }

    VkmDriverMetal::~VkmDriverMetal()
    {
        // Deliberately the one thing this destructor does. Everything else the driver owns is
        // released by destroy(), but several unit-test fixtures only `delete driver`, and a
        // leaked MTL4CounterHeap is not merely wasteful: once enough of them accumulate in one
        // process, newCounterHeapWithDescriptor: segfaults inside the AGX driver rather than
        // returning nil. Releasing twice is safe -- destroyGpuTimestampPool() nils the handle.
        destroyGpuTimestampPool();
    }

    VkmInitResult VkmDriverMetal::initializeInner(const VkmEngineLaunchOptions* options)
    {
        if (_mtlDevice == nil)
        {
            VKM_DEBUG_ERROR("No Metal device available on this system.");
            return VkmInitResult{VkmInitResultCode::HardwareUnsupported, "No Metal device available on this system."};
        }

        if (@available(macOS 26.0, iOS 26.0, *))
        {
            if (![_mtlDevice supportsFamily:MTLGPUFamilyApple9])
            {
                VKM_DEBUG_ERROR("Metal 4 requires Apple Silicon (MTLGPUFamilyApple9 or later). Non-Apple Silicon devices are not supported.");
                return VkmInitResult{VkmInitResultCode::HardwareUnsupported, "Metal 4 requires Apple Silicon (MTLGPUFamilyApple9 or later); this GPU does not support it."};
            }
        }
        else
        {
            VKM_DEBUG_ERROR("Metal 4 requires macOS 26 / iOS 26 or later. This OS version is not supported.");
            return VkmInitResult{VkmInitResultCode::HardwareUnsupported, "Metal 4 requires macOS 26 / iOS 26 or later; this OS version is not supported."};
        }

        // Queried rather than assumed, so the texture storage-mode policy stays honest if the
        // MTLGPUFamilyApple9 gate above is ever relaxed: an Intel Mac with a discrete GPU reports
        // false here.
        _hasUnifiedMemory = [_mtlDevice hasUnifiedMemory];

        // MTLBuffer.gpuAddress is unconditional on Metal; the argument buffers and push constant
        // ring bind by address.
        _driverCapabilityFlags = VkmDriverCapabilityFlags::TextureContentCapture | VkmDriverCapabilityFlags::TextureUpload |
                                 VkmDriverCapabilityFlags::BindlessTextures |
                                 VkmDriverCapabilityFlags::BufferDeviceAddress;
        if (_hasUnifiedMemory)
        {
            _driverCapabilityFlags = _driverCapabilityFlags | VkmDriverCapabilityFlags::TextureHostCopy;
        }
        // Asked of the device rather than assumed from the Metal version: Metal 4 runs on
        // hardware that predates hardware ray tracing (Apple 5 and earlier), where the API
        // exists but supportsRaytracing is false.
        if ([_mtlDevice supportsRaytracing])
        {
            _driverCapabilityFlags = _driverCapabilityFlags | VkmDriverCapabilityFlags::RayTracing;
        }
        /*
        * Placement sparse: a hardware tier, not an API version. Queried through a throwaway
        * descriptor because sparseTextureTier is a property of a texture's configuration rather
        * than of the device -- the same reason the tile size query takes a texture type and format.
        * Tier 1 is enough for per-mip residency: partial backing plus defined reads of unbacked
        * texels. Tier 2 adds per-tile access counters, which this does not use.
        */
        {
            MTLTextureDescriptor* probe = [[MTLTextureDescriptor alloc] init];
            probe.textureType = MTLTextureType2D;
            probe.pixelFormat = MTLPixelFormatRGBA8Unorm;
            probe.width = 256;
            probe.height = 256;
            probe.mipmapLevelCount = 9;
            probe.usage = MTLTextureUsageShaderRead;
            probe.storageMode = MTLStorageModePrivate;
            probe.placementSparsePageSize = kSparsePageSize;

            id<MTLTexture> probeTexture = [_mtlDevice newTextureWithDescriptor:probe];
            const MTLTextureSparseTier tier =
                (probeTexture != nil) ? [probeTexture sparseTextureTier] : MTLTextureSparseTierNone;
            if (tier != MTLTextureSparseTierNone)
            {
                _driverCapabilityFlags = _driverCapabilityFlags | VkmDriverCapabilityFlags::SparseResidency;
            }
            [probeTexture release];
            [probe release];
        }

        // Metal-4 MetalFX support is its own device question, separate from classic MetalFX.
        // Withheld while the Metal debug layer is active: MTL4FXTemporalScaler's encode aborts
        // the process under the debug layer's wrapper objects, so the capability stays clear
        // there and callers take their non-upscaled path instead.
        const char* debugLayer = getenv("MTL_DEBUG_LAYER");
        const bool debugLayerActive = debugLayer != nullptr && debugLayer[0] != '\0' && debugLayer[0] != '0';
        if (!debugLayerActive && [MTLFXTemporalScalerDescriptor supportsMetal4FX:_mtlDevice])
        {
            _driverCapabilityFlags = _driverCapabilityFlags | VkmDriverCapabilityFlags::TemporalUpscaling;
        }
        else if (debugLayerActive)
        {
            VKM_DEBUG_INFO("Temporal upscaling withheld: MTL4FX cannot encode under MTL_DEBUG_LAYER");
        }

        // Allocates no heap block yet -- the first Heap resource grows the first one.
        _heapAllocator = std::make_unique<VkmGpuHeapAllocatorMetal>(this);
        return VkmInitResult{VkmInitResultCode::Success, ""};
    }

    bool VkmDriverMetal::postInitializeInner()
    {
        // Runs after the render resource pool (residency sets) and command queues exist --
        // the manager registers its shared buffers into the Default residency set.
        auto bindlessResourceManager = std::make_unique<VkmBindlessResourceManagerMetal>(this);
        if (!bindlessResourceManager->initialize())
        {
            VKM_DEBUG_ERROR("Failed to initialize Metal bindless resource manager");
            return false;
        }
        _bindlessResourceManager = std::move(bindlessResourceManager);

        auto frameConstantManager = std::make_unique<VkmFrameConstantManagerMetal>(this);
        if (!frameConstantManager->initialize())
        {
            VKM_DEBUG_ERROR("Failed to initialize Metal frame constant manager");
            return false;
        }
        _frameConstantManager = std::move(frameConstantManager);

#if defined(VKM_GPU_CAPTURE)
        if (getLaunchOptions().enableGpuCapture)
        {
            // Frame-aligned capture scope on the Graphics MTL4 queue. Set as the default
            // capture scope so Xcode's Metal capture button records a bounded frame
            // instead of being unavailable/unbounded for the MTL4 workload.
            VkmCommandQueueMetal* graphicsQueue = static_cast<VkmCommandQueueMetal*>(
                getCommandQueue(VkmCommandQueueType::Graphics, 0));
            MTLCaptureManager* captureManager = [MTLCaptureManager sharedCaptureManager];
            _captureScope = [captureManager newCaptureScopeWithMTL4CommandQueue:graphicsQueue->getMTLCommandQueue()];
            if (_captureScope != nil)
            {
                _captureScope.label = @"vkm frame";
                captureManager.defaultCaptureScope = _captureScope;
            }
            else
            {
                // Capture is tooling support -- never fatal.
                VKM_DEBUG_WARN("Failed to create MTLCaptureScope; GPU frame capture unavailable");
            }
        }
#endif // VKM_GPU_CAPTURE
        return true;
    }

#if defined(VKM_GPU_CAPTURE)
    void VkmDriverMetal::onFrameBegin()
    {
        if (_captureScope == nil)
        {
            return;
        }

        if (!_programmaticCaptureActive && _captureFramesRemaining > 0)
        {
            if (_captureStartCountdown > 0)
            {
                // Requested to start N frames later -- not this frame yet.
                --_captureStartCountdown;
            }
            else
            {
                MTLCaptureManager* captureManager = [MTLCaptureManager sharedCaptureManager];
                if (![captureManager supportsDestination:MTLCaptureDestinationGPUTraceDocument])
                {
                    VKM_DEBUG_ERROR(".gputrace capture unsupported: launch with --enable-gpu-capture "
                                    "(sets MTL_CAPTURE_ENABLED=1) or set MTL_CAPTURE_ENABLED=1 in the shell");
                    _captureFramesRemaining = 0;
                }
                else
                {
                    char timestamp[32];
                    const std::time_t now = std::time(nullptr);
                    std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", std::localtime(&now));
                    const std::string tracePath =
                        (std::filesystem::current_path() / (std::string("vkm_capture_") + timestamp + ".gputrace")).string();

                    MTLCaptureDescriptor* captureDescriptor = [[MTLCaptureDescriptor alloc] init];
                    // Capture the device, not _captureScope: with MTL_CAPTURE_ENABLED=1 the
                    // GPUToolsCapture layer throws "-[MTLCaptureScope traceStream]:
                    // unrecognized selector" for scopes created via
                    // newCaptureScopeWithMTL4CommandQueue:. The capture window is still
                    // frame-exact because startCapture/stopCapture bracket whole
                    // onFrameBegin()/onFrameEnd() pairs (frameCount consecutive ones).
                    captureDescriptor.captureObject = _mtlDevice;
                    captureDescriptor.destination = MTLCaptureDestinationGPUTraceDocument;
                    captureDescriptor.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:tracePath.c_str()]];
                    NSError* error = nil;
                    if ([captureManager startCaptureWithDescriptor:captureDescriptor error:&error])
                    {
                        _programmaticCaptureActive = true;
                        _pendingTracePath = tracePath;
                    }
                    else
                    {
                        VKM_DEBUG_ERROR(fmt::format("startCaptureWithDescriptor failed: {}",
                            mtlErrorToString(error)).c_str());
                        _captureFramesRemaining = 0;
                    }
                    [captureDescriptor release]; // MRC
                }
            }
        }

        // Must follow startCaptureWithDescriptor so this scope iteration is recorded.
        [_captureScope beginScope];
    }

    void VkmDriverMetal::onFrameEnd()
    {
        if (_captureScope == nil)
        {
            return;
        }
        [_captureScope endScope];

        if (_programmaticCaptureActive && --_captureFramesRemaining == 0)
        {
            [[MTLCaptureManager sharedCaptureManager] stopCapture];
            _programmaticCaptureActive = false;
            VKM_DEBUG_INFO(fmt::format("GPU frame capture written to {}", _pendingTracePath).c_str());
            _pendingTracePath.clear();
        }
    }

    void VkmDriverMetal::requestGpuFrameCapture(uint32_t startFrameDelay, uint32_t frameCount)
    {
        if (_captureScope == nil)
        {
            VKM_DEBUG_WARN("GPU frame capture requires --enable-gpu-capture at launch");
            return;
        }
        if (_programmaticCaptureActive || _captureFramesRemaining > 0)
        {
            VKM_DEBUG_WARN("GPU frame capture already pending/active; ignoring new request");
            return;
        }
        _captureStartCountdown = startFrameDelay;
        _captureFramesRemaining = std::max<uint32_t>(frameCount, 1);
    }
#endif // VKM_GPU_CAPTURE

    void VkmDriverMetal::destroyInner()
    {
#if defined(VKM_GPU_CAPTURE)
        if (_captureScope != nil)
        {
            MTLCaptureManager* captureManager = [MTLCaptureManager sharedCaptureManager];
            if (_programmaticCaptureActive)
            {
                [captureManager stopCapture];
                _programmaticCaptureActive = false;
            }
            // The defaultCaptureScope property retains the scope -- clear it before
            // releasing our +1 ownership (MRC).
            if (captureManager.defaultCaptureScope == _captureScope)
            {
                captureManager.defaultCaptureScope = nil;
            }
            [_captureScope release];
            _captureScope = nil;
        }
#endif // VKM_GPU_CAPTURE

        if (_frameConstantManager)
        {
            _frameConstantManager->destroy();
            _frameConstantManager.reset();
        }

        if (_bindlessResourceManager)
        {
            _bindlessResourceManager->destroy();
            _bindlessResourceManager.reset();
        }

        // After the resource teardown above: a heap block outlives the resources placed in it,
        // and releasing it first would drop the memory they still reference.
        if (_heapAllocator)
        {
            _heapAllocator->destroy();
            _heapAllocator.reset();
        }
    }

    VkmTexture* VkmDriverMetal::newTextureInner()
    {
        // TODO(snowapril) : create texture via resource pool backend
        return new VkmTextureMetal(this);
    }

    VkmBuffer* VkmDriverMetal::newBufferInner()
    {
        return new VkmBufferMetal(this);
    }

    VkmStagingBuffer* VkmDriverMetal::newStagingBufferInner()
    {
        return new VkmStagingBufferMetal(this);
    }

    VkmSampler* VkmDriverMetal::newSamplerInner()
    {
        return new VkmSamplerMetal(this);
    }

    VkmTextureView* VkmDriverMetal::newTextureViewInner()
    {
        return new VkmTextureViewMetal(this);
    }

    VkmBufferView* VkmDriverMetal::newBufferViewInner()
    {
        return new VkmBufferViewMetal(this);
    }

    bool VkmDriverMetal::onCreateAliasBlock(uint32_t blockIndex, uint64_t sizeBytes, uint32_t memoryTypeBits)
    {
        // Metal has no memory-type concept; VkmAliasedMemoryHeap passes ~0u so every block is
        // compatible with every texture.
        (void)memoryTypeBits;

        // VkmAliasedMemoryHeap hands out indices in order and never reuses one, so a mismatch
        // means the two have fallen out of step rather than that a block is missing.
        if (blockIndex != (uint32_t)_imageHeaps.size())
        {
            VKM_DEBUG_ERROR("Aliasing block indices are out of step with the driver's heap list");
            return false;
        }

        auto heap = std::make_unique<VkmGpuImageHeapMetal>(this);
        if (!heap->initialize(sizeBytes))
        {
            return false;
        }

        // A placed texture does not make its heap resident on its own, exactly as for the buffer
        // pool above -- register the block so every texture in it is covered by one entry.
        VkmRenderResourcePoolMetal* renderResourcePoolMetal =
            static_cast<VkmRenderResourcePoolMetal*>(getRenderResourcePool());
        renderResourcePoolMetal->registerExternalAllocation(heap->getHeap());

        _imageHeaps.push_back(std::move(heap));
        return true;
    }

    void VkmDriverMetal::onDestroyAliasBlock(uint32_t blockIndex)
    {
        // Destroyed in place rather than erased: indices are the heap's identity for a block and
        // must stay stable for the placements still naming them.
        if (blockIndex < (uint32_t)_imageHeaps.size() && _imageHeaps[blockIndex] != nullptr)
        {
            VkmRenderResourcePoolMetal* renderResourcePoolMetal =
                static_cast<VkmRenderResourcePoolMetal*>(getRenderResourcePool());
            renderResourcePoolMetal->unregisterExternalAllocation(_imageHeaps[blockIndex]->getHeap());
            _imageHeaps[blockIndex]->destroy();
        }
    }

    id<MTLTexture> VkmDriverMetal::createTextureInAliasBlock(uint32_t blockIndex, MTLTextureDescriptor* descriptor,
                                                             uint64_t offset)
    {
        if (blockIndex >= (uint32_t)_imageHeaps.size() || _imageHeaps[blockIndex] == nullptr)
        {
            VKM_DEBUG_ERROR("An aliased texture was placed into a block that does not exist");
            return nil;
        }
        return _imageHeaps[blockIndex]->createTexture(descriptor, offset);
    }

    VkmGpuMemoryStats VkmDriverMetal::getGpuMemoryStats() const
    {
        VkmGpuMemoryStats stats{};
        if (_mtlDevice == nil)
        {
            return stats;
        }

        stats._deviceAllocatedBytes = static_cast<uint64_t>([_mtlDevice currentAllocatedSize]);
        stats._deviceBudgetBytes = static_cast<uint64_t>([_mtlDevice recommendedMaxWorkingSetSize]);
        stats._hasDeviceStats = true;

        if (_heapAllocator)
        {
            _heapAllocator->accumulateMemoryStats(&stats);
        }

        // Aliasing blocks are the engine's suballocator too, and counting them here is what makes
        // the trade visible: the per-texture tags of an aliased texture report zero, so without
        // this the bytes a block reserves would appear nowhere at all.
        for (const auto& imageHeap : _imageHeaps)
        {
            id<MTLHeap> heap = imageHeap != nullptr ? imageHeap->getHeap() : nil;
            if (heap == nil)
            {
                continue;
            }
            stats._poolReservedBytes += static_cast<uint64_t>([heap currentAllocatedSize]);
            stats._poolUsedBytes += static_cast<uint64_t>([heap usedSize]);
            stats._hasPoolStats = true;
        }

        return stats;
    }

    bool VkmDriverMetal::initializeGpuTimestampPool(const uint32_t slotCount)
    {
        if (_mtlDevice == nil)
        {
            return false;
        }

        MTL4CounterHeapDescriptor* descriptor = [[MTL4CounterHeapDescriptor alloc] init]; // MRC
        descriptor.type = MTL4CounterHeapTypeTimestamp;
        descriptor.count = slotCount;
        NSError* counterHeapError = nil;
        _timestampCounterHeap = [_mtlDevice newCounterHeapWithDescriptor:descriptor error:&counterHeapError];
        [descriptor release]; // MRC

        if (_timestampCounterHeap == nil)
        {
            // Recoverable: GPU profiling turns itself off, the engine keeps running. Logged at info
            // level for that reason, but with the reason Metal gave rather than a bare message.
            VKM_DEBUG_INFO(fmt::format("Failed to create the GPU timestamp counter heap ({}); GPU profiling is disabled",
                mtlErrorToString(counterHeapError)).c_str());
            return false;
        }
        _timestampCounterHeap.label = @"VkmGpuProfilerTimestamps";

        // Queried rather than assumed to be nanoseconds: the heap stores raw GPU ticks, and
        // their rate is a device property.
        const uint64_t timestampFrequency = [_mtlDevice queryTimestampFrequency];
        if (timestampFrequency == 0)
        {
            VKM_DEBUG_INFO("Metal reports a zero GPU timestamp frequency; GPU profiling is disabled");
            destroyGpuTimestampPool();
            return false;
        }
        _timestampPeriodNs = 1'000'000'000.0 / static_cast<double>(timestampFrequency);

        _driverCapabilityFlags = _driverCapabilityFlags | VkmDriverCapabilityFlags::TimestampQuery;
        return true;
    }

    void VkmDriverMetal::destroyGpuTimestampPool()
    {
        if (_timestampCounterHeap != nil)
        {
            [_timestampCounterHeap release]; // MRC
            _timestampCounterHeap = nil;
        }
    }

    void VkmDriverMetal::resetGpuTimestampSlots(VkmCommandBufferBase* commandBuffer, const uint32_t firstSlot,
                                                const uint32_t count)
    {
        // Nothing is recorded into the command buffer: invalidating a counter range takes effect
        // immediately on the CPU timeline, and the profiler only ever resets slots whose previous
        // submission has already been resolved.
        (void)commandBuffer;
        if (_timestampCounterHeap == nil || count == 0)
        {
            return;
        }
        [_timestampCounterHeap invalidateCounterRange:NSMakeRange(firstSlot, count)];
    }

    bool VkmDriverMetal::resolveGpuTimestamps(const uint32_t firstSlot, const uint32_t count, uint64_t* outTicks)
    {
        if (_timestampCounterHeap == nil || count == 0 || outTicks == nullptr)
        {
            return false;
        }

        // CPU-timeline resolve, which is only valid once the writing work has finished -- the
        // profiler guarantees that by waiting on the submission's timeline first. This avoids the
        // GPU-side resolveCounterHeap path and the buffer + fence it would need.
        NSData* resolved = [_timestampCounterHeap resolveCounterRange:NSMakeRange(firstSlot, count)];
        if (resolved == nil || resolved.length < sizeof(MTL4TimestampHeapEntry) * count)
        {
            return false;
        }

        const MTL4TimestampHeapEntry* entries = static_cast<const MTL4TimestampHeapEntry*>(resolved.bytes);
        for (uint32_t index = 0; index < count; ++index)
        {
            outTicks[index] = entries[index].timestamp;
        }
        return true;
    }

    VkmPipelineStateBase* VkmDriverMetal::newPipelineStateInner()
    {
        return new VkmPipelineStateMetal(this);
    }

    VkmAccelerationStructure* VkmDriverMetal::newAccelerationStructureInner()
    {
        return new VkmAccelerationStructureMetal(this);
    }

    VkmUpscalerBase* VkmDriverMetal::newUpscalerInner()
    {
        return new VkmUpscalerMetal();
    }

    VkmResourceTableBase* VkmDriverMetal::newResourceTableInner()
    {
        return new VkmResourceTableMetal(this);
    }

    VkmSwapChainBase* VkmDriverMetal::newSwapChainInner()
    {
        return new VkmSwapChainMetal(this);
    }

    VkmCommandQueueBase* VkmDriverMetal::newCommandQueueInner()
    {
        return new VkmCommandQueueMetal(this);
    }

    VkmRenderResourcePool* VkmDriverMetal::newRenderResourcePoolInner()
    {
        return new VkmRenderResourcePoolMetal(this);
    }

    VkmFormat VkmDriverMetal::selectSwapChainColorFormat(bool enableHdr) const
    {
        // HDR only when requested AND the display can actually present extended dynamic range,
        // otherwise the non-HDR 8-bit format (which matches how BGRA8 pipelines expect to write).
        if (enableHdr)
        {
#if TARGET_OS_OSX
            // Ensure AppKit is initialized before querying the screen (driver init runs before
            // NSApplicationMain). A nil screen -> treat as no EDR (safe non-HDR fallback).
            [NSApplication sharedApplication];
            NSScreen* mainScreen = [NSScreen mainScreen];
            if (mainScreen != nil && mainScreen.maximumPotentialExtendedDynamicRangeColorComponentValue > 1.0)
            {
                return VkmFormat::R16G16B16A16_SFLOAT;
            }
#endif // TARGET_OS_OSX
        }
        return VkmFormat::BGRA8_UNORM;
    }
}