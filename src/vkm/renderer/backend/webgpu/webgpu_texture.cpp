// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_texture.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>

#include <algorithm>

namespace vkm
{
    namespace
    {
        WGPUTextureUsage toWGPUTextureUsage(VkmResourceCreateInfo flags)
        {
            uint64_t usage = WGPUTextureUsage_None;
            if ((flags & VkmResourceCreateInfo::AllowTransferSrc) != 0) usage |= WGPUTextureUsage_CopySrc;
            if ((flags & VkmResourceCreateInfo::AllowTransferDst) != 0) usage |= WGPUTextureUsage_CopyDst;
            if ((flags & VkmResourceCreateInfo::AllowShaderRead) != 0) usage |= WGPUTextureUsage_TextureBinding;
            if ((flags & VkmResourceCreateInfo::AllowShaderWrite) != 0) usage |= WGPUTextureUsage_StorageBinding;
            if ((flags & (VkmResourceCreateInfo::AllowColorAttachment | VkmResourceCreateInfo::AllowDepthStencilAttachment | VkmResourceCreateInfo::AllowPresent)) != 0)
                usage |= WGPUTextureUsage_RenderAttachment;
            return static_cast<WGPUTextureUsage>(usage);
        }
    } // namespace

    VkmTextureWebGPU::VkmTextureWebGPU(VkmDriverBase* driver)
        : VkmTexture(driver)
    {
    }

    VkmTextureWebGPU::~VkmTextureWebGPU()
    {
        if (_wgpuTexture != nullptr && _externallyOwned == false)
        {
            wgpuTextureRelease(_wgpuTexture);
        }
        _wgpuTexture = nullptr;
    }

    bool VkmTextureWebGPU::initialize(VkmResourceHandle handle, const VkmTextureInfo& info)
    {
        if (!initializeTextureCommon(handle, info))
        {
            return false;
        }

        // Swapchain back buffers are supplied later via overrideExternalHandle; don't create our own.
        if ((info._flags & VkmResourceCreateInfo::DeferredCreation) == 0 &&
            (info._flags & VkmResourceCreateInfo::ExternalHandleOwner) == 0)
        {
            VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);

            const WGPUTextureDescriptor textureDesc{
                .label         = toWGPUStringView("VkmTextureWebGPU"),
                .usage         = toWGPUTextureUsage(info._flags),
                .dimension     = WGPUTextureDimension_2D,
                .size          = WGPUExtent3D{info._extent.x, info._extent.y, std::max(1u, info._numArrayLayers)},
                .format        = toWGPUTextureFormat(info._format),
                .mipLevelCount = std::max(1u, info._numMipLevels),
                .sampleCount   = 1,
            };
            _wgpuTexture = wgpuDeviceCreateTexture(driverWebGPU->getDevice(), &textureDesc);
        }

        return true;
    }

    bool VkmTextureWebGPU::overrideExternalHandle(void* externalHandle)
    {
        _wgpuTexture = static_cast<WGPUTexture>(externalHandle);
        _externallyOwned = true;
        return true;
    }

    void VkmTextureWebGPU::setDebugName(const char* name)
    {
        if (_wgpuTexture != nullptr)
        {
            wgpuTextureSetLabel(_wgpuTexture, toWGPUStringView(name));
        }
    }
} // namespace vkm
