// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>
#include <vkm/renderer/backend/webgpu/webgpu_swapchain.h>
#include <vkm/renderer/backend/webgpu/webgpu_texture.h>
#include <vkm/renderer/backend/webgpu/webgpu_command_queue.h>
#include <vkm/renderer/backend/webgpu/webgpu_pipeline_state.h>
#include <vkm/renderer/engine.h>

#include <emscripten/emscripten.h>

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
        const WGPUUncapturedErrorCallbackInfo errorCallbackInfo{
            .callback = logWGPUUncapturedError,
        };
        const WGPUDeviceDescriptor deviceDesc{
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

    void VkmDriverWebGPU::destroyInner()
    {
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

    VkmPipelineStateBase* VkmDriverWebGPU::newPipelineStateInner()
    {
        return new VkmPipelineStateWebGPU(this);
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
