// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_buffer_view.h>
#include <vkm/renderer/backend/vulkan/vulkan_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

namespace vkm
{
    VkmBufferViewVulkan::VkmBufferViewVulkan(VkmDriverBase* driver)
        : VkmBufferView(driver)
    {
    }

    VkmBufferViewVulkan::~VkmBufferViewVulkan()
    {
        if (_vkBufferView != VK_NULL_HANDLE)
        {
            VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
            vkDestroyBufferView(driverVulkan->getDevice(), _vkBufferView, nullptr);
            _vkBufferView = VK_NULL_HANDLE;
        }
    }

    bool VkmBufferViewVulkan::initialize(VkmResourceHandle handle, const VkmBufferViewInfo& info)
    {
        if (!initializeBufferViewCommon(handle, info))
        {
            return false;
        }

        if (info._buffer.type != VkmResourceType::Buffer)
        {
            VKM_DEBUG_ERROR("VkmBufferViewInfo::_buffer must reference a Buffer handle");
            return false;
        }

        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        VkmBufferVulkan* parentBuffer = static_cast<VkmBufferVulkan*>(resolveParent());
        if (parentBuffer == nullptr)
        {
            return false;
        }

        // The parent buffer may itself be a sub-range of a shared pool VkBuffer; the
        // absolute offset for binding/view-creation must account for both.
        _absoluteOffset = parentBuffer->getBufferOffset() + info._offset;

        if (info._format == VkmFormat::Undefined)
        {
            // Metadata-only: regular UBO/SSBO bindings use offset+range directly, no
            // VkBufferView needed.
            return true;
        }

        const VkBufferViewCreateInfo viewCreateInfo{
            .sType  = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .buffer = parentBuffer->getBuffer(),
            .format = toVkFormat(info._format),
            .offset = _absoluteOffset,
            .range  = (info._size == UINT64_MAX) ? VK_WHOLE_SIZE : info._size,
        };
        const VkResult vkResult = vkCreateBufferView(driverVulkan->getDevice(), &viewCreateInfo, nullptr, &_vkBufferView);
        return VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create buffer view");
    }

    void VkmBufferViewVulkan::setDebugName(const char* name)
    {
#ifdef VKM_DEBUG_NAME_ENABLED
        if (_vkBufferView == VK_NULL_HANDLE)
        {
            return;
        }
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        const VkDebugUtilsObjectNameInfoEXT nameInfo{
            .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType   = VK_OBJECT_TYPE_BUFFER_VIEW,
            .objectHandle = reinterpret_cast<uint64_t>(_vkBufferView),
            .pObjectName  = name,
        };
        vkSetDebugUtilsObjectNameEXT(driverVulkan->getDevice(), &nameInfo);
#else
        (void)name;
#endif
    }
} // namespace vkm
