// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <webgpu/webgpu.h>

#include <string>

namespace vkm
{
    inline WGPUStringView toWGPUStringView(const char* str)
    {
        return WGPUStringView{str, WGPU_STRLEN};
    }

    std::string toStdString(WGPUStringView view);

    WGPULoadOp toWGPULoadOp(VkmLoadAction loadAction);
    WGPUStoreOp toWGPUStoreOp(VkmStoreAction storeAction);
    WGPUTextureFormat toWGPUTextureFormat(VkmFormat format);
    VkmFormat fromWGPUTextureFormat(WGPUTextureFormat format);

    WGPUBufferUsage toWGPUBufferUsage(VkmResourceCreateInfo flags);
    WGPUAddressMode toWGPUAddressMode(VkmAddressMode addressMode);
    WGPUFilterMode toWGPUFilterMode(VkmFilterMode filterMode);
    WGPUMipmapFilterMode toWGPUMipmapFilterMode(VkmMipmapMode mipmapMode);
    WGPUCompareFunction toWGPUCompareFunction(VkmCompareOp compareOp);

    void logWGPUUncapturedError(WGPUDevice const* device, WGPUErrorType type, WGPUStringView message, void* userdata1, void* userdata2);

    /*
    * @brief Routes a real GPU crash/error to VkmGpuCrashHandler::reportCrash(). Ignores
    * WGPUDeviceLostReason_Destroyed -- that reason also fires on the engine's own ordinary
    * device teardown (VkmDriverWebGPU::destroyInner()), which is not a crash.
    * userdata1 must be the owning VkmDriverWebGPU*, set when registering
    * WGPUDeviceDescriptor::deviceLostCallbackInfo.
    */
    void onWGPUDeviceLost(WGPUDevice const* device, WGPUDeviceLostReason reason, WGPUStringView message, void* userdata1, void* userdata2);
} // namespace vkm
