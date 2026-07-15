// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_swapchain.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_texture.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

namespace vkm
{
    VkmSwapChainWebGPU::VkmSwapChainWebGPU(VkmDriverBase* driver)
        : VkmSwapChainBase(driver)
    {
    }

    VkmSwapChainWebGPU::~VkmSwapChainWebGPU()
    {
    }

    void VkmSwapChainWebGPU::setDebugName(const char* name)
    {
        if (_surface != nullptr)
        {
            wgpuSurfaceSetLabel(_surface, toWGPUStringView(name));
        }

        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        for (const VkmResourceHandle& handle : _backBuffers)
        {
            if (handle.isValid())
            {
                VkmTexture* texture = renderResourcePool->getResource<VkmTexture>(handle);
                texture->setDebugName(name);
            }
        }
    }

    bool VkmSwapChainWebGPU::createSwapChain(void* windowHandle)
    {
        (void)windowHandle; // GLFWwindow*; the canvas is selected by CSS selector below, matching the id
                            // GLFW's Emscripten port assigns to the default canvas (see platform/wasm/AGENTS.md)

        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);

        WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSelector{};
        canvasSelector.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
        canvasSelector.selector = toWGPUStringView("#canvas");

        const WGPUSurfaceDescriptor surfaceDesc{
            .nextInChain = &canvasSelector.chain,
            .label       = toWGPUStringView("VkmSwapChainWebGPU Surface"),
        };
        _surface = wgpuInstanceCreateSurface(driverWebGPU->getInstance(), &surfaceDesc);
        if (_surface == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create WebGPU surface from canvas");
            return false;
        }

        WGPUSurfaceCapabilities capabilities{};
        if (wgpuSurfaceGetCapabilities(_surface, driverWebGPU->getAdapter(), &capabilities) != WGPUStatus_Success ||
            capabilities.formatCount == 0)
        {
            VKM_DEBUG_ERROR("Failed to query WebGPU surface capabilities");
            return false;
        }
        // formats[0] is the surface's preferred format, per the WebGPU spec.
        _surfaceFormat = capabilities.formats[0];
        wgpuSurfaceCapabilitiesFreeMembers(capabilities);

        const WGPUSurfaceConfiguration surfaceConfig{
            .device      = driverWebGPU->getDevice(),
            .format      = _surfaceFormat,
            .usage       = WGPUTextureUsage_RenderAttachment,
            .width       = _extent.x,
            .height      = _extent.y,
            .presentMode = WGPUPresentMode_Fifo, // most broadly supported present mode across browsers
        };
        wgpuSurfaceConfigure(_surface, &surfaceConfig);

        VkmTextureInfo textureInfo;
        textureInfo._flags = VkmResourceCreateInfo::ExternalHandleOwner | VkmResourceCreateInfo::AllowColorAttachment | VkmResourceCreateInfo::AllowPresent;
        textureInfo._extent = glm::uvec3(_extent.x, _extent.y, 1);
        textureInfo._format = VkmFormat::R8G8B8A8_UNORM; // TODO(snowapril): VkmFormat has no BGRA variant; purely descriptive since ExternalHandleOwner skips creation
        textureInfo._numMipLevels = 1;
        textureInfo._numArrayLayers = 1;

        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        for (VkmResourceHandle& handle : _backBuffers)
        {
            VkmTextureWebGPU* newTextureWebGPU = new VkmTextureWebGPU(_driver);
            handle = renderResourcePool->allocateTexture(newTextureWebGPU);
            if (newTextureWebGPU->initialize(handle, textureInfo) == false)
            {
                VKM_DEBUG_ERROR("Failed to create swap chain texture");
                if (handle.isValid())
                    renderResourcePool->releaseResource(handle);
                else
                    delete newTextureWebGPU;

                destroySwapChain();
                return false;
            }
        }

        return true;
    }

    void VkmSwapChainWebGPU::destroySwapChain()
    {
        if (_surface != nullptr)
        {
            wgpuSurfaceUnconfigure(_surface);
            wgpuSurfaceRelease(_surface);
            _surface = nullptr;
        }

        destroySwapChainCommon();
    }

    VkmResourceHandle VkmSwapChainWebGPU::acquireNextImageInner()
    {
        WGPUSurfaceTexture surfaceTexture{};
        wgpuSurfaceGetCurrentTexture(_surface, &surfaceTexture);

        if (surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
            surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
        {
            VKM_DEBUG_ERROR(fmt::format("Failed to acquire current WebGPU surface texture (status={})", (int)surfaceTexture.status).c_str());
            return VKM_INVALID_RESOURCE_HANDLE;
        }

        _currentBackBufferIndex = (_currentBackBufferIndex + 1) % BACK_BUFFER_COUNT;

        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        VkmTextureWebGPU* textureWebGPU = static_cast<VkmTextureWebGPU*>(renderResourcePool->getResource<VkmTexture>(_backBuffers[_currentBackBufferIndex]));
        const bool result = textureWebGPU->overrideExternalHandle(surfaceTexture.texture);
        VKM_ASSERT(result, "Failed to override external handle");
        return textureWebGPU->getHandle();
    }

    void VkmSwapChainWebGPU::presentInner()
    {
        // Nothing to do: under Emscripten the browser presents the canvas implicitly at
        // the end of each requestAnimationFrame tick (the emscripten_set_main_loop driver
        // in platform/wasm), and emdawnwebgpu's wgpuSurfacePresent aborts with
        // "unsupported (use requestAnimationFrame via html5.h instead)" if called.
    }
} // namespace vkm
