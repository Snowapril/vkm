// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_resource_table.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>
#include <vkm/renderer/backend/webgpu/webgpu_swapchain.h>
#include <vkm/renderer/backend/webgpu/webgpu_texture.h>
#include <vkm/renderer/backend/webgpu/webgpu_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_staging_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_sampler.h>
#include <vkm/renderer/backend/webgpu/webgpu_texture_view.h>
#include <vkm/renderer/backend/webgpu/webgpu_buffer_view.h>
#include <vkm/renderer/backend/webgpu/webgpu_command_queue.h>
#include <vkm/renderer/backend/webgpu/webgpu_pipeline_state.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/engine.h>

#include <emscripten/emscripten.h>

#include <cstring>

namespace vkm
{
    namespace
    {
        // wgpuInstanceRequestAdapter/wgpuAdapterRequestDevice are asynchronous on Web.
        // WGPUCallbackMode_AllowSpontaneous lets them fire from the browser event loop;
        // emscripten_sleep (requires -sASYNCIFY) yields to that event loop so initializeInner
        // can keep its synchronous bool-returning contract without touching VkmDriverBase.
        void onRequestAdapterEnded(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* userdata1, void* userdata2)
        {
            if (status != WGPURequestAdapterStatus_Success)
            {
                VKM_DEBUG_ERROR(fmt::format("Failed to request WebGPU adapter: {}", toStdString(message)).c_str());
            }
            *static_cast<WGPUAdapter*>(userdata1) = adapter;
            *static_cast<bool*>(userdata2) = true;
        }

        struct MapAsyncResult
        {
            bool done = false;
            WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
        };

        void onTimestampBufferMapped(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void*)
        {
            auto* result = static_cast<MapAsyncResult*>(userdata1);
            result->status = status;
            result->done = true;
            if (status != WGPUMapAsyncStatus_Success)
            {
                VKM_DEBUG_ERROR(fmt::format("Failed to map the GPU timestamp readback buffer: {}",
                                            toStdString(message)).c_str());
            }
        }

        void onRequestDeviceEnded(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void* userdata2)
        {
            if (status != WGPURequestDeviceStatus_Success)
            {
                VKM_DEBUG_ERROR(fmt::format("Failed to request WebGPU device: {}", toStdString(message)).c_str());
            }
            *static_cast<WGPUDevice*>(userdata1) = device;
            *static_cast<bool*>(userdata2) = true;
        }
    } // namespace

    VkmDriverWebGPU::VkmDriverWebGPU()
        : VkmDriverBase()
    {
    }

    VkmDriverWebGPU::~VkmDriverWebGPU()
    {
    }

    VkmInitResult VkmDriverWebGPU::initializeInner(const VkmEngineLaunchOptions* options)
    {
        (void)options;

        const WGPUInstanceDescriptor instanceDesc{};
        _instance = wgpuCreateInstance(&instanceDesc);
        if (_instance == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create WebGPU instance");
            return VkmInitResult{VkmInitResultCode::Failed, "Failed to create WebGPU instance"};
        }

        bool adapterRequestDone = false;
        const WGPURequestAdapterOptions adapterOptions{
            .powerPreference = WGPUPowerPreference_HighPerformance,
        };
        const WGPURequestAdapterCallbackInfo adapterCallbackInfo{
            .mode      = WGPUCallbackMode_AllowSpontaneous,
            .callback  = onRequestAdapterEnded,
            .userdata1 = &_adapter,
            .userdata2 = &adapterRequestDone,
        };
        wgpuInstanceRequestAdapter(_instance, &adapterOptions, adapterCallbackInfo);
        while (adapterRequestDone == false)
        {
            emscripten_sleep(1);
        }

        if (_adapter == nullptr)
        {
            VKM_DEBUG_ERROR("No WebGPU adapter available");
            return VkmInitResult{VkmInitResultCode::HardwareUnsupported, "No WebGPU adapter available on this system."};
        }

        WGPUAdapterInfo adapterInfo{};
        if (wgpuAdapterGetInfo(_adapter, &adapterInfo) == WGPUStatus_Success)
        {
            VKM_DEBUG_INFO(fmt::format("Selected WebGPU adapter: {} ({})", toStdString(adapterInfo.device), toStdString(adapterInfo.description)).c_str());
            wgpuAdapterInfoFreeMembers(adapterInfo);
        }

        bool deviceRequestDone = false;
        const WGPUDeviceLostCallbackInfo deviceLostCallbackInfo{
            .mode      = WGPUCallbackMode_AllowSpontaneous,
            .callback  = onWGPUDeviceLost,
            .userdata1 = this,
        };
        const WGPUUncapturedErrorCallbackInfo errorCallbackInfo{
            .callback = logWGPUUncapturedError,
        };
        // timestamp-query is optional in WebGPU, so it must be both offered by the adapter and
        // asked for at device creation; requesting an unsupported feature fails device creation
        // outright, which is why this is conditional rather than unconditional.
        _timestampQuerySupported = wgpuAdapterHasFeature(_adapter, WGPUFeatureName_TimestampQuery);
        const WGPUFeatureName requiredFeatures[] = { WGPUFeatureName_TimestampQuery };
        const WGPUDeviceDescriptor deviceDesc{
            .requiredFeatureCount = _timestampQuerySupported ? 1u : 0u,
            .requiredFeatures = _timestampQuerySupported ? requiredFeatures : nullptr,
            .deviceLostCallbackInfo = deviceLostCallbackInfo,
            .uncapturedErrorCallbackInfo = errorCallbackInfo,
        };
        const WGPURequestDeviceCallbackInfo deviceCallbackInfo{
            .mode      = WGPUCallbackMode_AllowSpontaneous,
            .callback  = onRequestDeviceEnded,
            .userdata1 = &_device,
            .userdata2 = &deviceRequestDone,
        };
        wgpuAdapterRequestDevice(_adapter, &deviceDesc, deviceCallbackInfo);
        while (deviceRequestDone == false)
        {
            emscripten_sleep(1);
        }

        if (_device == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create WebGPU device");
            return VkmInitResult{VkmInitResultCode::Failed, "Failed to create WebGPU device"};
        }

        _queue = wgpuDeviceGetQueue(_device);

        _driverCapabilityFlags = VkmDriverCapabilityFlags::None;

        return VkmInitResult{VkmInitResultCode::Success, ""};
    }

    bool VkmDriverWebGPU::postInitializeInner()
    {
        // Runs after the resource pool and command queues exist; the manager submits
        // transient copies to the device queue when buffers are registered.
        auto bindlessResourceManager = std::make_unique<VkmBindlessResourceManagerWebGPU>(this);
        if (!bindlessResourceManager->initialize())
        {
            VKM_DEBUG_ERROR("Failed to initialize WebGPU bindless resource manager");
            return false;
        }
        _bindlessResourceManager = std::move(bindlessResourceManager);

        auto frameConstantManager = std::make_unique<VkmFrameConstantManagerWebGPU>(this);
        if (!frameConstantManager->initialize())
        {
            VKM_DEBUG_ERROR("Failed to initialize WebGPU frame constant manager");
            return false;
        }
        _frameConstantManager = std::move(frameConstantManager);
        return true;
    }

    void VkmDriverWebGPU::destroyInner()
    {
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
        if (_queue != nullptr)
        {
            wgpuQueueRelease(_queue);
            _queue = nullptr;
        }
        if (_device != nullptr)
        {
            wgpuDeviceRelease(_device);
            _device = nullptr;
        }
        if (_adapter != nullptr)
        {
            wgpuAdapterRelease(_adapter);
            _adapter = nullptr;
        }
        if (_instance != nullptr)
        {
            wgpuInstanceRelease(_instance);
            _instance = nullptr;
        }
    }

    VkmTexture* VkmDriverWebGPU::newTextureInner()
    {
        return new VkmTextureWebGPU(this);
    }

    VkmBuffer* VkmDriverWebGPU::newBufferInner()
    {
        return new VkmBufferWebGPU(this);
    }

    VkmStagingBuffer* VkmDriverWebGPU::newStagingBufferInner()
    {
        return new VkmStagingBufferWebGPU(this);
    }

    VkmSampler* VkmDriverWebGPU::newSamplerInner()
    {
        return new VkmSamplerWebGPU(this);
    }

    VkmTextureView* VkmDriverWebGPU::newTextureViewInner()
    {
        return new VkmTextureViewWebGPU(this);
    }

    VkmBufferView* VkmDriverWebGPU::newBufferViewInner()
    {
        return new VkmBufferViewWebGPU(this);
    }

    VkmRenderResourcePool* VkmDriverWebGPU::newRenderResourcePoolInner()
    {
        return new VkmRenderResourcePool(this);
    }

    bool VkmDriverWebGPU::initializeGpuTimestampPool(const uint32_t slotCount)
    {
        if (_timestampQuerySupported == false)
        {
            VKM_DEBUG_INFO("WebGPU adapter does not offer timestamp-query; GPU profiling is disabled");
            return false;
        }

        const WGPUQuerySetDescriptor querySetDesc{
            .label = toWGPUStringView("VkmGpuProfilerTimestamps"),
            .type  = WGPUQueryType_Timestamp,
            .count = slotCount,
        };
        _timestampQuerySet = wgpuDeviceCreateQuerySet(_device, &querySetDesc);
        if (_timestampQuerySet == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create the WebGPU timestamp query set; GPU profiling is disabled");
            return false;
        }

        // A query set can only be read through a GPU-side resolve into a QueryResolve buffer,
        // and a QueryResolve buffer can never also be MapRead -- hence the second, mappable copy.
        const uint64_t resolveSize = static_cast<uint64_t>(slotCount) * sizeof(uint64_t);
        const WGPUBufferDescriptor resolveDesc{
            .label = toWGPUStringView("VkmGpuProfilerTimestampResolve"),
            .usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc,
            .size  = resolveSize,
        };
        _timestampResolveBuffer = wgpuDeviceCreateBuffer(_device, &resolveDesc);

        const WGPUBufferDescriptor readbackDesc{
            .label = toWGPUStringView("VkmGpuProfilerTimestampReadback"),
            .usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead,
            .size  = resolveSize,
        };
        _timestampReadbackBuffer = wgpuDeviceCreateBuffer(_device, &readbackDesc);

        if (_timestampResolveBuffer == nullptr || _timestampReadbackBuffer == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create the WebGPU timestamp readback buffers; GPU profiling is disabled");
            destroyGpuTimestampPool();
            return false;
        }

        _driverCapabilityFlags = _driverCapabilityFlags | VkmDriverCapabilityFlags::TimestampQuery;
        return true;
    }

    void VkmDriverWebGPU::destroyGpuTimestampPool()
    {
        if (_timestampReadbackBuffer != nullptr)
        {
            wgpuBufferRelease(_timestampReadbackBuffer);
            _timestampReadbackBuffer = nullptr;
        }
        if (_timestampResolveBuffer != nullptr)
        {
            wgpuBufferRelease(_timestampResolveBuffer);
            _timestampResolveBuffer = nullptr;
        }
        if (_timestampQuerySet != nullptr)
        {
            wgpuQuerySetRelease(_timestampQuerySet);
            _timestampQuerySet = nullptr;
        }
    }

    bool VkmDriverWebGPU::resolveGpuTimestamps(const uint32_t firstSlot, const uint32_t count, uint64_t* outTicks)
    {
        if (_timestampReadbackBuffer == nullptr || count == 0 || outTicks == nullptr)
        {
            return false;
        }

        // The GPU-side resolve+copy was already recorded by VkmCommandBufferWebGPU::
        // onResolveGpuZones, and the caller only reaches here once that submission completed, so
        // this map returns immediately -- it is the same blocking map readbackTexture already
        // performs through VkmStagingBufferWebGPU.
        // size_t rather than uint64_t: the map/get-range entry points take size_t, which is 32-bit
        // on wasm.
        const size_t offset = static_cast<size_t>(firstSlot) * sizeof(uint64_t);
        const size_t size = static_cast<size_t>(count) * sizeof(uint64_t);

        MapAsyncResult result;
        const WGPUBufferMapCallbackInfo callbackInfo{
            .mode      = WGPUCallbackMode_AllowSpontaneous,
            .callback  = onTimestampBufferMapped,
            .userdata1 = &result,
        };
        wgpuBufferMapAsync(_timestampReadbackBuffer, WGPUMapMode_Read, offset, size, callbackInfo);
        while (result.done == false)
        {
            emscripten_sleep(1);
        }
        if (result.status != WGPUMapAsyncStatus_Success)
        {
            return false;
        }

        const void* mapped = wgpuBufferGetConstMappedRange(_timestampReadbackBuffer, offset, size);
        if (mapped == nullptr)
        {
            wgpuBufferUnmap(_timestampReadbackBuffer);
            return false;
        }
        std::memcpy(outTicks, mapped, size);
        wgpuBufferUnmap(_timestampReadbackBuffer);
        return true;
    }

    VkmPipelineStateBase* VkmDriverWebGPU::newPipelineStateInner()
    {
        return new VkmPipelineStateWebGPU(this);
    }

    VkmFormat VkmDriverWebGPU::selectSwapChainColorFormat(bool enableHdr) const
    {
        // TODO(hdr): WebGPU HDR support is limited; use the non-HDR format for now. The surface's
        // preferred format is typically BGRA8, so this stays consistent with createSwapChain.
        (void)enableHdr;
        return VkmFormat::BGRA8_UNORM;
    }

    VkmResourceTableBase* VkmDriverWebGPU::newResourceTableInner()
    {
        return new VkmResourceTableWebGPU(this);
    }

    VkmSwapChainBase* VkmDriverWebGPU::newSwapChainInner()
    {
        return new VkmSwapChainWebGPU(this);
    }

    VkmCommandQueueBase* VkmDriverWebGPU::newCommandQueueInner()
    {
        return new VkmCommandQueueWebGPU(this);
    }
} // namespace vkm
