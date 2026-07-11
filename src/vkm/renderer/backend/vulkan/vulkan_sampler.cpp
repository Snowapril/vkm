// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_sampler.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>

namespace vkm
{
    VkmSamplerVulkan::VkmSamplerVulkan(VkmDriverBase* driver)
        : VkmSampler(driver)
    {
    }

    VkmSamplerVulkan::~VkmSamplerVulkan()
    {
        if (_vkSampler != VK_NULL_HANDLE)
        {
            VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
            vkDestroySampler(driverVulkan->getDevice(), _vkSampler, nullptr);
            _vkSampler = VK_NULL_HANDLE;
        }
    }

    bool VkmSamplerVulkan::initialize(VkmResourceHandle handle, const VkmSamplerInfo& info)
    {
        if (!initializeSamplerCommon(handle, info))
        {
            return false;
        }

        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);

        // Samplers have no memory backing at all per the Vulkan spec -- no VMA involvement.
        const VkSamplerCreateInfo samplerCreateInfo{
            .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter               = toVkFilter(info._magFilter),
            .minFilter               = toVkFilter(info._minFilter),
            .mipmapMode              = toVkSamplerMipmapMode(info._mipmapMode),
            .addressModeU            = toVkSamplerAddressMode(info._addressModeU),
            .addressModeV            = toVkSamplerAddressMode(info._addressModeV),
            .addressModeW            = toVkSamplerAddressMode(info._addressModeW),
            .anisotropyEnable        = info._maxAnisotropy > 1.0f,
            .maxAnisotropy           = info._maxAnisotropy,
            .compareEnable           = info._compareEnable,
            .compareOp               = toVkCompareOp(info._compareOp),
            .minLod                  = info._minLod,
            .maxLod                  = info._maxLod,
            .borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };

        const VkResult vkResult = vkCreateSampler(driverVulkan->getDevice(), &samplerCreateInfo, nullptr, &_vkSampler);
        return VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create sampler");
    }

    void VkmSamplerVulkan::setDebugName(const char* name)
    {
#ifdef VKM_DEBUG_NAME_ENABLED
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        const VkDebugUtilsObjectNameInfoEXT nameInfo{
            .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType   = VK_OBJECT_TYPE_SAMPLER,
            .objectHandle = reinterpret_cast<uint64_t>(_vkSampler),
            .pObjectName  = name,
        };
        vkSetDebugUtilsObjectNameEXT(driverVulkan->getDevice(), &nameInfo);
#else
        (void)name;
#endif
    }
} // namespace vkm
