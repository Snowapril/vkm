// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_sampler.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>

namespace vkm
{
    VkmSamplerWebGPU::VkmSamplerWebGPU(VkmDriverBase* driver)
        : VkmSampler(driver)
    {
    }

    VkmSamplerWebGPU::~VkmSamplerWebGPU()
    {
        if (_wgpuSampler != nullptr)
        {
            wgpuSamplerRelease(_wgpuSampler);
        }
        _wgpuSampler = nullptr;
    }

    bool VkmSamplerWebGPU::initialize(VkmResourceHandle handle, const VkmSamplerInfo& info)
    {
        if (!initializeSamplerCommon(handle, info))
        {
            return false;
        }

        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);

        const WGPUSamplerDescriptor samplerDesc{
            .label         = toWGPUStringView("VkmSamplerWebGPU"),
            .addressModeU  = toWGPUAddressMode(info._addressModeU),
            .addressModeV  = toWGPUAddressMode(info._addressModeV),
            .addressModeW  = toWGPUAddressMode(info._addressModeW),
            .magFilter     = toWGPUFilterMode(info._magFilter),
            .minFilter     = toWGPUFilterMode(info._minFilter),
            .mipmapFilter  = toWGPUMipmapFilterMode(info._mipmapMode),
            .lodMinClamp   = info._minLod,
            .lodMaxClamp   = info._maxLod,
            .compare       = info._compareEnable ? toWGPUCompareFunction(info._compareOp) : WGPUCompareFunction_Undefined,
            .maxAnisotropy = (info._maxAnisotropy > 1.0f) ? (uint16_t)info._maxAnisotropy : 1,
        };
        _wgpuSampler = wgpuDeviceCreateSampler(driverWebGPU->getDevice(), &samplerDesc);
        if (_wgpuSampler == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create WGPUSampler");
            return false;
        }
        return true;
    }

    void VkmSamplerWebGPU::setDebugName(const char* name)
    {
        if (_wgpuSampler != nullptr)
        {
            wgpuSamplerSetLabel(_wgpuSampler, toWGPUStringView(name));
        }
    }
} // namespace vkm
