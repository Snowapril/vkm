// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_buffer.h>
#include <vkm/renderer/backend/metal/metal_staging_buffer.h>
#include <vkm/renderer/backend/metal/metal_sampler.h>
#include <vkm/renderer/backend/metal/metal_texture_view.h>
#include <vkm/renderer/backend/metal/metal_buffer_view.h>
#include <vkm/renderer/backend/metal/metal_gpu_heap_pool.h>
#include <vkm/renderer/backend/metal/metal_swapchain.h>
#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <vkm/renderer/backend/metal/metal_pipeline_state.h>
#include <vkm/renderer/backend/metal/metal_render_resource_pool.h>

#import <Metal/MTLDevice.h>
#import <Metal/MTLHeap.h>
#import <Metal/MTLCaptureManager.h>
#import <Metal/MTLCaptureScope.h>

#include <ctime>
#include <filesystem>

namespace vkm
{
    VkmDriverMetal::VkmDriverMetal(id<MTLDevice> mtlDevice)
        : VkmDriverBase(), _mtlDevice(mtlDevice)
    {
        
    }

    VkmDriverMetal::~VkmDriverMetal()
    {

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

        _driverCapabilityFlags = VkmDriverCapabilityFlags::TextureContentCapture;
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
        return true;
    }

    void VkmDriverMetal::onFrameBegin()
    {
        if (_captureScope == nil)
        {
            return;
        }

        if (_gpuFrameCaptureRequested && !_programmaticCaptureActive)
        {
            _gpuFrameCaptureRequested = false;

            MTLCaptureManager* captureManager = [MTLCaptureManager sharedCaptureManager];
            if (![captureManager supportsDestination:MTLCaptureDestinationGPUTraceDocument])
            {
                VKM_DEBUG_ERROR(".gputrace capture unsupported: launch with --enable-gpu-capture "
                                "(sets MTL_CAPTURE_ENABLED=1) or set MTL_CAPTURE_ENABLED=1 in the shell");
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
                // newCaptureScopeWithMTL4CommandQueue:. The capture is still exactly one
                // frame because startCapture/stopCapture bracket a single
                // onFrameBegin()/onFrameEnd() pair.
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
                        error != nil ? error.localizedDescription.UTF8String : "unknown error").c_str());
                }
                [captureDescriptor release]; // MRC
            }
        }

        // Must follow startCaptureWithDescriptor so exactly this scope iteration is recorded.
        [_captureScope beginScope];
    }

    void VkmDriverMetal::onFrameEnd()
    {
        if (_captureScope == nil)
        {
            return;
        }
        [_captureScope endScope];

        if (_programmaticCaptureActive)
        {
            [[MTLCaptureManager sharedCaptureManager] stopCapture];
            _programmaticCaptureActive = false;
            VKM_DEBUG_INFO(fmt::format("GPU frame capture written to {}", _pendingTracePath).c_str());
            _pendingTracePath.clear();
        }
    }

    void VkmDriverMetal::requestGpuFrameCapture()
    {
        if (_captureScope == nil)
        {
            VKM_DEBUG_WARN("GPU frame capture requires --enable-gpu-capture at launch");
            return;
        }
        _gpuFrameCaptureRequested = true;
    }

    void VkmDriverMetal::destroyInner()
    {
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

        if (_bindlessResourceManager)
        {
            _bindlessResourceManager->destroy();
            _bindlessResourceManager.reset();
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

    id<MTLBuffer> VkmDriverMetal::allocateFromHeapPool(uint64_t sizeBytes, uint64_t alignment, uint64_t options)
    {
        for (auto& pool : _heapPools)
        {
            id<MTLBuffer> buffer = pool->tryAllocateBuffer(sizeBytes, alignment, options);
            if (buffer != nil)
            {
                return buffer;
            }
        }

        if (sizeBytes > VkmGpuHeapPoolMetal::POOL_BLOCK_SIZE_BYTES)
        {
            VKM_DEBUG_ERROR("Buffer allocation exceeds heap pool block size; use a committed allocation instead");
            return nil;
        }

        auto newPool = std::make_unique<VkmGpuHeapPoolMetal>(this);
        if (!newPool->initialize())
        {
            return nil;
        }

        // Placed sub-allocations may not make the backing heap resident on their own;
        // register the whole heap block so every buffer placed in it is covered.
        VkmRenderResourcePoolMetal* renderResourcePoolMetal = static_cast<VkmRenderResourcePoolMetal*>(getRenderResourcePool());
        renderResourcePoolMetal->registerExternalAllocation(newPool->getHeap());

        id<MTLBuffer> buffer = newPool->tryAllocateBuffer(sizeBytes, alignment, options);
        _heapPools.push_back(std::move(newPool));
        return buffer;
    }

    VkmPipelineStateBase* VkmDriverMetal::newPipelineStateInner()
    {
        return new VkmPipelineStateMetal(this);
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
}