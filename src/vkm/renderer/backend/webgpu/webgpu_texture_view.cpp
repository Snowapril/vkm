// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_texture_view.h>
#include <vkm/renderer/backend/webgpu/webgpu_texture.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

namespace vkm
{
    VkmTextureViewWebGPU::VkmTextureViewWebGPU(VkmDriverBase* driver)
        : VkmTextureView(driver)
    {
    }

    VkmTextureViewWebGPU::~VkmTextureViewWebGPU()
    {
        if (_wgpuTextureView != nullptr)
        {
            wgpuTextureViewRelease(_wgpuTextureView);
        }
        _wgpuTextureView = nullptr;
    }

    bool VkmTextureViewWebGPU::initialize(VkmResourceHandle handle, const VkmTextureViewInfo& info)
    {
        if (!initializeTextureViewCommon(handle, info))
        {
            return false;
        }

        if (info._texture.type != VkmResourceType::Texture)
        {
            VKM_DEBUG_ERROR("VkmTextureViewInfo::_texture must reference a Texture handle");
            return false;
        }

        VkmTextureWebGPU* parentTexture = static_cast<VkmTextureWebGPU*>(resolveParent());
        if (parentTexture == nullptr)
        {
            return false;
        }

        const VkmTextureInfo& parentInfo = parentTexture->getTextureInfo();
        const VkmFormat resolvedFormat = (info._format != VkmFormat::Undefined) ? info._format : parentInfo._format;
        const uint32_t numMipLevels = (info._numMipLevels != UINT32_MAX)
            ? info._numMipLevels
            : (parentInfo._numMipLevels - info._baseMipLevel);
        const uint32_t numArrayLayers = (info._numArrayLayers != UINT32_MAX)
            ? info._numArrayLayers
            : (parentInfo._numArrayLayers - info._baseArrayLayer);

        const WGPUTextureViewDescriptor viewDesc{
            .label           = toWGPUStringView("VkmTextureViewWebGPU"),
            .format          = toWGPUTextureFormat(resolvedFormat),
            .dimension       = (numArrayLayers > 1) ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D,
            .baseMipLevel    = info._baseMipLevel,
            .mipLevelCount   = numMipLevels,
            .baseArrayLayer  = info._baseArrayLayer,
            .arrayLayerCount = numArrayLayers,
            .aspect          = WGPUTextureAspect_All,
        };
        _wgpuTextureView = wgpuTextureCreateView(parentTexture->getWGPUTexture(), &viewDesc);
        if (_wgpuTextureView == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create WGPUTextureView");
            return false;
        }
        return true;
    }

    void VkmTextureViewWebGPU::setDebugName(const char* name)
    {
        if (_wgpuTextureView != nullptr)
        {
            wgpuTextureViewSetLabel(_wgpuTextureView, toWGPUStringView(name));
        }
    }
} // namespace vkm
