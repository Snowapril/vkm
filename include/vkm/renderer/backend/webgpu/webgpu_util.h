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

    void logWGPUUncapturedError(WGPUDevice const* device, WGPUErrorType type, WGPUStringView message, void* userdata1, void* userdata2);
} // namespace vkm
