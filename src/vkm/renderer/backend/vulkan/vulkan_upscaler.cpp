// Copyright (c) 2025 Snowapril

#if defined(VKM_ENABLE_FSR)

#include <vkm/renderer/backend/vulkan/vulkan_upscaler.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>

// After the Vulkan backend headers, which pull in volk: ffx_api_vk.h includes vulkan.h, and the
// volk-provided one (prototype-free) must win the include-guard race.
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

#if defined(VKM_PLATFORM_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <string>

namespace vkm
{
    namespace
    {
        // fpMessage bridge: the runtime reports wide strings, the vkm logger takes narrow ones.
        void ffxMessageToVkmLog(uint32_t type, const wchar_t* message)
        {
            std::string narrow;
            for (const wchar_t* c = message; c != nullptr && *c != L'\0'; ++c)
            {
                narrow.push_back(*c < 128 ? static_cast<char>(*c) : '?');
            }
            if (type == FFX_API_MESSAGE_TYPE_ERROR)
            {
                VKM_DEBUG_ERROR(("FSR: " + narrow).c_str());
            }
            else
            {
                VKM_DEBUG_WARN(("FSR: " + narrow).c_str());
            }
        }

        FfxApiResource makeFfxTexture(VkmDriverBase* driver, VkmResourceHandle handle,
                                      uint32_t usage, uint32_t state)
        {
            VkmTexture* texture = driver->getRenderResourcePool()->getResource<VkmTexture>(handle);
            if (texture == nullptr)
            {
                return FfxApiResource{};
            }
            VkmTextureVulkan* textureVulkan = static_cast<VkmTextureVulkan*>(texture);
            const VkmTextureInfo& info = texture->getTextureInfo();

            FfxApiResourceDescription description{};
            description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
            description.format = ffxApiGetSurfaceFormatVK(toVkFormat(info._format));
            description.width = info._extent.x;
            description.height = info._extent.y;
            description.depth = 1;
            description.mipCount = info._numMipLevels;
            description.flags = 0;
            description.usage = usage;
            return ffxApiGetResourceVK(textureVulkan->getImage(), description, state);
        }
    } // namespace

    bool VkmUpscalerVulkan::initializeInner()
    {
#if !defined(VKM_PLATFORM_WINDOWS)
        VKM_DEBUG_ERROR("FSR upscaler: only the Windows prebuilt runtime is wired up");
        return false;
#else
        HMODULE module = LoadLibraryA("amd_fidelityfx_vk.dll");
        if (module == nullptr)
        {
            VKM_DEBUG_ERROR(("FSR upscaler: amd_fidelityfx_vk.dll not found next to the binary "
                             "(LoadLibrary error " + std::to_string(GetLastError()) + ")")
                                .c_str());
            return false;
        }
        _ffxModule = module;
        _createContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module, "ffxCreateContext"));
        _destroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module, "ffxDestroyContext"));
        _dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module, "ffxDispatch"));
        if (_createContext == nullptr || _destroyContext == nullptr || _dispatch == nullptr)
        {
            VKM_DEBUG_ERROR("FSR upscaler: amd_fidelityfx_vk.dll is missing ffx-api entry points");
            destroyInner();
            return false;
        }

        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        ffxCreateBackendVKDesc backendDesc{};
        backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backendDesc.vkDevice = driverVulkan->getDevice();
        backendDesc.vkPhysicalDevice = driverVulkan->getPhysicalDevice();
        backendDesc.vkDeviceProcAddr = vkGetDeviceProcAddr; // volk-loaded

        ffxCreateContextDescUpscale createDesc{};
        createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        createDesc.header.pNext = &backendDesc.header;
        createDesc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
        if (_descriptor._autoExposure)
        {
            createDesc.flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        }
        if (_descriptor._depthInverted)
        {
            createDesc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        }
        createDesc.maxRenderSize = { _descriptor._renderExtent.x, _descriptor._renderExtent.y };
        createDesc.maxUpscaleSize = { _descriptor._displayExtent.x, _descriptor._displayExtent.y };
        createDesc.fpMessage = &ffxMessageToVkmLog;

        const ffxReturnCode_t result = _createContext(&_context, &createDesc.header, nullptr);
        if (result != FFX_API_RETURN_OK)
        {
            VKM_DEBUG_ERROR(("FSR upscaler: ffxCreateContext failed with code " +
                             std::to_string(result))
                                .c_str());
            _context = nullptr;
            destroyInner();
            return false;
        }
        return true;
#endif // VKM_PLATFORM_WINDOWS
    }

    void VkmUpscalerVulkan::destroyInner()
    {
#if defined(VKM_PLATFORM_WINDOWS)
        if (_context != nullptr && _destroyContext != nullptr)
        {
            const ffxReturnCode_t result = _destroyContext(&_context, nullptr);
            if (result != FFX_API_RETURN_OK)
            {
                VKM_DEBUG_ERROR(("FSR upscaler: ffxDestroyContext failed with code " +
                                 std::to_string(result))
                                    .c_str());
            }
            _context = nullptr;
        }
        if (_ffxModule != nullptr)
        {
            FreeLibrary(static_cast<HMODULE>(_ffxModule));
            _ffxModule = nullptr;
        }
        _createContext = nullptr;
        _destroyContext = nullptr;
        _dispatch = nullptr;
#endif // VKM_PLATFORM_WINDOWS
    }

    void VkmUpscalerVulkan::encodeInner(VkmCommandBufferBase* commandBuffer,
                                        const VkmUpscalerDispatchDesc& dispatchDesc)
    {
        VkmCommandBufferVulkan* commandBufferVulkan = static_cast<VkmCommandBufferVulkan*>(commandBuffer);

        ffxDispatchDescUpscale desc{};
        desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        desc.commandList = commandBufferVulkan->getVkCommandBuffer();
        // States mirror the layouts the graph's barrier plan placed: ShaderSampledRead landed the
        // inputs in SHADER_READ_ONLY_OPTIMAL (COMPUTE_READ), ShaderStorageReadWrite landed the
        // output in GENERAL (UNORDERED_ACCESS). The runtime restores these states after its own
        // internal transitions, so the graph's per-subresource layout tracking stays truthful.
        desc.color = makeFfxTexture(_driver, dispatchDesc._color, FFX_API_RESOURCE_USAGE_READ_ONLY,
                                    FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.depth = makeFfxTexture(_driver, dispatchDesc._depth, FFX_API_RESOURCE_USAGE_DEPTHTARGET,
                                    FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.motionVectors = makeFfxTexture(_driver, dispatchDesc._motion, FFX_API_RESOURCE_USAGE_READ_ONLY,
                                            FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.output = makeFfxTexture(_driver, dispatchDesc._output, FFX_API_RESOURCE_USAGE_UAV,
                                     FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        if (desc.color.resource == nullptr || desc.depth.resource == nullptr ||
            desc.motionVectors.resource == nullptr || desc.output.resource == nullptr)
        {
            VKM_DEBUG_ERROR("FSR upscaler: a dispatch texture handle no longer resolves");
            return;
        }
        desc.jitterOffset = { dispatchDesc._jitterPixels.x, dispatchDesc._jitterPixels.y };
        // vkm motion is current-to-previous in UV units (+y down); FSR wants pixels pointing at
        // the pixel's previous location, so the UV-to-pixel scale is negated on both axes.
        desc.motionVectorScale = { -static_cast<float>(_descriptor._renderExtent.x),
                                   -static_cast<float>(_descriptor._renderExtent.y) };
        desc.renderSize = { _descriptor._renderExtent.x, _descriptor._renderExtent.y };
        desc.upscaleSize = { _descriptor._displayExtent.x, _descriptor._displayExtent.y };
        desc.enableSharpening = false;
        desc.sharpness = 0.0f;
        desc.frameTimeDelta = dispatchDesc._deltaTimeSeconds * 1000.0f;
        desc.preExposure = 1.0f;
        desc.reset = dispatchDesc._reset;
        desc.cameraNear = dispatchDesc._nearZ;
        desc.cameraFar = dispatchDesc._farZ;
        desc.cameraFovAngleVertical = dispatchDesc._fovYRadians;
        desc.viewSpaceToMetersFactor = 1.0f;
        desc.flags = 0;

        const ffxReturnCode_t result = _dispatch(&_context, &desc.header);
        if (result != FFX_API_RETURN_OK)
        {
            VKM_DEBUG_ERROR(("FSR upscaler: ffxDispatch failed with code " + std::to_string(result))
                                .c_str());
        }
    }
} // namespace vkm

#endif // VKM_ENABLE_FSR
