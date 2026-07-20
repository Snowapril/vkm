// Copyright (c) 2025 Snowapril

#include <vkm/renderer/engine.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph_capture.h>
#include <cxxopts.hpp>
#include <iostream>

#if defined(VKM_USE_VULKAN_API)
#include <vkm/renderer/backend/vulkan/vulkan_swapchain.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#elif defined(VKM_USE_METAL_API)
#include <vkm/renderer/backend/metal/metal_swapchain.h>
#elif defined(VKM_USE_WEBGPU_API)
#include <vkm/renderer/backend/webgpu/webgpu_swapchain.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>
#endif

#if defined(VKM_ENABLE_IMGUI)
#include <vkm/renderer/imgui/imgui_renderer.h>
#if defined(VKM_USE_VULKAN_API)
#include <vkm/renderer/imgui/vulkan_imgui_renderer.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_gpu_timer.h>
#elif defined(VKM_USE_METAL_API)
#include <vkm/renderer/imgui/metal_imgui_renderer.h>
#elif defined(VKM_USE_WEBGPU_API)
#include <vkm/renderer/imgui/webgpu_imgui_renderer.h>
#endif
#include <vkm/platform/common/process_stats.h>
#include <vkm/renderer/imgui/render_graph_inspector.h>
#include <imgui.h>
#endif

namespace vkm
{
    VkmEngine::VkmEngine(VkmDriverBase* driver)
        : _driver(driver), _lastUpdateTime(0.0)
    {
    }

    VkmEngine::~VkmEngine()
    {
    }

    bool VkmEngine::initializeEngine(AppDelegate* appDelegate, VkmEngineLaunchOptions options)
    {
        bool result = LoggerManager::singleton().initialize();
        if (!result)
        {
            std::cerr << "Failed to initialize logger manager" << std::endl;
            return false;
        }
        VKM_DEBUG_INFO("LoggerManager initialized");

        _appDelegate.reset(appDelegate);
        _engineOptions = options;

        for (uint8_t i = 0; i < FRAME_COUNT; ++i)
        {
            _frameRenderGraphs[i] = std::make_unique<VkmRenderGraph>(_driver, i);
        }

        _renderGraphCapture = std::make_unique<VkmRenderGraphCapture>();
        if (_engineOptions.captureRenderGraphOnStartup)
        {
            _renderGraphCapture->arm();
        }
#if defined(VKM_ENABLE_IMGUI)
        _renderGraphInspector = std::make_unique<VkmRenderGraphInspector>();
#endif

        return true;
    }
    
    VkmInitResult VkmEngine::initializeBackendDriver()
    {
        VkmInitResult result = _driver->initialize(&_engineOptions);
        if (result.code != VkmInitResultCode::Success)
        {
            VKM_DEBUG_ERROR(fmt::format("Failed to initialize renderer backend driver: {}", result.reason).c_str());
            return result;
        }
        VKM_DEBUG_INFO("Renderer backend driver initialized");

        _pipelineStateManager = std::make_unique<VkmPipelineStateManager>(_driver);
        std::string psoError;
        if (!_pipelineStateManager->loadPipelineStatesFromDirectory(
                std::string(RESOURCES_DIR) + "Pipelines/Engine/",
                std::string(RESOURCES_DIR) + "Shaders/ShaderCache/",
                VkmPipelineStateOrigin::Engine, &psoError))
        {
            VKM_DEBUG_ERROR(fmt::format("Failed to load engine pipeline states: {}", psoError).c_str());
            return VkmInitResult{VkmInitResultCode::Failed, psoError};
        }

#if defined(VKM_GPU_CAPTURE)
        // Must run after driver init -- the Metal capture scope is created there, and
        // requestGpuFrameCapture() is a no-op before it exists.
        if (_engineOptions.captureGpuFrameOnStartup)
        {
            _driver->requestGpuFrameCapture(_engineOptions.gpuCaptureStartFrame, _engineOptions.gpuCaptureFrameCount);
        }
#endif // VKM_GPU_CAPTURE

        _appDelegate->postDriverReady(this);

        return result;
    }

    void VkmEngine::loopInner(const double currentUpdateTime)
    {
        const double deltaTime = currentUpdateTime - _lastUpdateTime;
        _lastUpdateTime = currentUpdateTime;

#if defined(VKM_GPU_CAPTURE)
        // Frame-boundary driver hooks (MTLCaptureScope begin/end on Metal) bracket all of
        // this frame's encoding, submission, and present.
        _driver->onFrameBegin();
#endif // VKM_GPU_CAPTURE

#if defined(VKM_ENABLE_IMGUI)
        _imGuiRenderer->newFrame();
#endif

        update( deltaTime );
        render( deltaTime );

#if defined(VKM_GPU_CAPTURE)
        _driver->onFrameEnd();
#endif // VKM_GPU_CAPTURE

        _currentFrameIndex = (_currentFrameIndex + 1) % FRAME_COUNT;
    }

    void VkmEngine::destroy()
    {
        if (_renderGraphCapture)
        {
            _renderGraphCapture->releaseResources(_driver);
        }
#if defined(VKM_ENABLE_IMGUI)
        if (_imGuiRenderer)
        {
            _imGuiRenderer->shutdown();
            _imGuiRenderer.reset();
        }
#endif

        if (_mainSwapChain != nullptr)
        {
            delete _mainSwapChain;
            _mainSwapChain = nullptr;
        }
    }

    void VkmEngine::addSwapChain(const VkmWindowInfo& windowInfo)
    {
        VkmSwapChainBase* swapChain = _driver->newSwapChain();
        const bool result = swapChain->initialize(windowInfo);
        VKM_ASSERT(result, "Failed to create swapchain");

        VKM_ASSERT(_mainSwapChain == nullptr, "Main swapchain already exists");
        _mainSwapChain = swapChain;

#if defined(VKM_ENABLE_IMGUI)
        VkmFormat backBufferFormat = VkmFormat::Undefined;
#if defined(VKM_USE_VULKAN_API)
        backBufferFormat = fromVkFormat(static_cast<VkmSwapChainVulkan*>(swapChain)->getImageFormat());
        _imGuiRenderer = std::make_unique<VkmImGuiRendererVulkan>(_driver);
#elif defined(VKM_USE_METAL_API)
        backBufferFormat = static_cast<VkmSwapChainMetal*>(swapChain)->getBackBufferFormat();
        _imGuiRenderer = std::make_unique<VkmImGuiRendererMetal>(_driver);
#elif defined(VKM_USE_WEBGPU_API)
        backBufferFormat = fromWGPUTextureFormat(static_cast<VkmSwapChainWebGPU*>(swapChain)->getSurfaceFormat());
        _imGuiRenderer = std::make_unique<VkmImGuiRendererWebGPU>(_driver);
#endif
        const bool imGuiInitialized = _imGuiRenderer->initialize(windowInfo._windowHandle, backBufferFormat);
        VKM_ASSERT(imGuiInitialized, "Failed to initialize ImGui renderer");
#endif
    }

    void VkmEngine::update(const double deltaTime)
    {
#if defined(VKM_ENABLE_IMGUI)
        // Must run before the frame's first ImGui::Render() call (triggered lazily by
        // VkmImGuiRendererBase::renderDrawData() in render() below) -- ImGui::Begin/End
        // calls made after that point in the same frame would be dropped.
        renderDebugOverlay(deltaTime);

        if (ImGui::IsKeyPressed(ImGuiKey_F10, false))
        {
            _renderGraphCapture->arm();
        }
#if defined(VKM_GPU_CAPTURE)
        if (ImGui::IsKeyPressed(ImGuiKey_F9, false))
        {
            _driver->requestGpuFrameCapture(_engineOptions.gpuCaptureStartFrame, _engineOptions.gpuCaptureFrameCount);
        }
#endif // VKM_GPU_CAPTURE
        _renderGraphInspector->draw(*_renderGraphCapture, _driver, _imGuiRenderer.get());
#endif
        _appDelegate->update(deltaTime);
    }

#if defined(VKM_ENABLE_IMGUI)
    void VkmEngine::renderDebugOverlay(const double deltaTime)
    {
        const double fps = (deltaTime > 0.0) ? (1.0 / deltaTime) : 0.0;
        _fpsSmoothed = _fpsSmoothed * 0.9 + fps * 0.1;

        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("Debug Overlay", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("FPS: %.1f", _fpsSmoothed);
        ImGui::Text("CPU: %.1f%%", getProcessCpuUsagePercent());
#if defined(VKM_USE_VULKAN_API)
        ImGui::Text("GPU: %.2f ms", static_cast<VkmDriverVulkan*>(_driver)->getGpuTimer()->getLastGpuFrameTimeMs());
#else
        ImGui::Text("GPU: n/a");
#endif
        ImGui::Text("Frame: %u", _currentFrameIndex);
        ImGui::Text("F10: capture render graph");
#if defined(VKM_USE_METAL_API) && defined(VKM_GPU_CAPTURE)
        ImGui::Text("F9: capture GPU frame (.gputrace)");
#endif
        ImGui::End();
    }
#endif

    void VkmEngine::prepareRender()
    {
        // Need to wait gpu
        VkmRenderGraph* renderGraph = _frameRenderGraphs[_currentFrameIndex].get();
        // Ensure the render graph is completed before proceeding
        renderGraph->ensureCompleted();
        // Reset the render graph for the current frame
        renderGraph->reset();
    }

    void VkmEngine::render(const double deltaTime)
    {
        // Throttle before acquiring: prepareRender() timeline-waits on this frame slot's previous
        // submit and resets its render graph. This must precede acquireNextImage() so the
        // image-available semaphore for this slot is guaranteed free before it is reused.
        prepareRender();

        VkmResourceHandle currentBackBuffer = _mainSwapChain->acquireNextImage();
        if (!currentBackBuffer.isValid())
        {
            // Acquire failed (e.g. surface lost/out-of-date). prepareRender() only waited on and
            // reset this frame slot, so there is no half-recorded work to unwind; skip the frame
            // rather than record and present a stale image index.
            return;
        }
        VKM_DEBUG_INFO(fmt::format("Engine update : delta time : {}", deltaTime).c_str());

        VkmRenderGraph* renderGraph = _frameRenderGraphs[_currentFrameIndex].get();

        _appDelegate->render(renderGraph, currentBackBuffer);

#if defined(VKM_ENABLE_IMGUI)
        // ImGui overlay: draws on top of whatever the app already recorded, loading (not
        // clearing) the same back buffer.
        VkmFrameBufferDescriptor imGuiFrameBufferDesc;
        imGuiFrameBufferDesc._renderPass._colorAttachmentCount = 1;
        imGuiFrameBufferDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        imGuiFrameBufferDesc._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::Load;
        imGuiFrameBufferDesc._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
        imGuiFrameBufferDesc._width = _mainSwapChain->getExtent().x;
        imGuiFrameBufferDesc._height = _mainSwapChain->getExtent().y;
        imGuiFrameBufferDesc._colorAttachments[0] = currentBackBuffer;

        VkmRenderGraphicsSubGraph* imGuiSubGraph = renderGraph->beginGraphicsSubGraph(imGuiFrameBufferDesc, "EngineImGuiOverlay");
        VkmImGuiRendererBase* imGuiRenderer = _imGuiRenderer.get();
        imGuiSubGraph->setRenderCallback([imGuiRenderer](VkmCommandBufferBase* commandBuffer) {
            imGuiRenderer->renderDrawData(commandBuffer);
        });

        // While a capture is inspectable, the ImGui pass may sample its snapshot textures --
        // reference them so recordUsage() tracks the in-flight draws and the deferred
        // reclaimer waits for them when the capture is released.
        if (_renderGraphCapture->getState() == VkmRenderGraphCapture::State::Ready)
        {
            for (VkmResourceHandle snapshotHandle : _renderGraphCapture->getSnapshotTextureHandles())
            {
                imGuiSubGraph->addReferencedResource(snapshotHandle);
            }
        }
#endif

        renderGraph->compile();

        VkmRenderGraphCapture* capture =
            (_renderGraphCapture->getState() == VkmRenderGraphCapture::State::Armed) ? _renderGraphCapture.get() : nullptr;
        renderGraph->execute(VkmRenderGraphCommitOptions{ .waitForCompletion = false, .presentSwapChain = _mainSwapChain, .capture = capture } );
        if (capture != nullptr)
        {
            // One deliberate hitch on the capture frame: the buffer readbacks and snapshot
            // copies must have completed on the GPU before finalize() maps them.
            renderGraph->ensureCompleted();
            capture->finalize(_driver);
        }

        _mainSwapChain->present();
    }

    VkmEngineLaunchOptions VkmEngine::parseEngineLaunchOptions(int argc, char* argv[])
    {
        cxxopts::Options options("vkm", "vkm engine launch options");
        options.allow_unrecognised_options();
        options.add_options()
            ("enable-validation-layer", "Enable the graphics validation layer",
                cxxopts::value<bool>()->default_value(DEFAULT_ENGINE_LAUNCH_OPTIONS.enableValidationLayer ? "true" : "false"))
            ("enable-gpu-capture", "Enable GPU capture tooling support (e.g. native debug labels for RenderDoc/Xcode)",
                cxxopts::value<bool>()->default_value(DEFAULT_ENGINE_LAUNCH_OPTIONS.enableGpuCapture ? "true" : "false"))
            ("enable-gpu-crash-dump", "Enable GPU crash handler submission-breadcrumb recording",
                cxxopts::value<bool>()->default_value(DEFAULT_ENGINE_LAUNCH_OPTIONS.enableGpuCrashDump ? "true" : "false"))
            ("capture-render-graph", "Arm a render graph capture at startup (first frame is captured)",
                cxxopts::value<bool>()->default_value(DEFAULT_ENGINE_LAUNCH_OPTIONS.captureRenderGraphOnStartup ? "true" : "false"))
            ("gpu-capture-frame", "Capture a .gputrace at startup (Metal; implies --enable-gpu-capture)",
                cxxopts::value<bool>()->default_value(DEFAULT_ENGINE_LAUNCH_OPTIONS.captureGpuFrameOnStartup ? "true" : "false"))
            ("gpu-capture-start-frame", "Start the GPU capture N frames after it is requested",
                cxxopts::value<uint32_t>()->default_value(std::to_string(DEFAULT_ENGINE_LAUNCH_OPTIONS.gpuCaptureStartFrame)))
            ("gpu-capture-frame-count", "Number of consecutive frames to record into the .gputrace",
                cxxopts::value<uint32_t>()->default_value(std::to_string(DEFAULT_ENGINE_LAUNCH_OPTIONS.gpuCaptureFrameCount)));

        VkmEngineLaunchOptions launchOptions = DEFAULT_ENGINE_LAUNCH_OPTIONS;
        try
        {
            auto result = options.parse(argc, argv);
            launchOptions.enableValidationLayer = result["enable-validation-layer"].as<bool>();
            launchOptions.enableGpuCapture = result["enable-gpu-capture"].as<bool>();
            launchOptions.enableGpuCrashDump = result["enable-gpu-crash-dump"].as<bool>();
            launchOptions.captureRenderGraphOnStartup = result["capture-render-graph"].as<bool>();
            launchOptions.captureGpuFrameOnStartup = result["gpu-capture-frame"].as<bool>();
            launchOptions.gpuCaptureStartFrame = result["gpu-capture-start-frame"].as<uint32_t>();
            launchOptions.gpuCaptureFrameCount = result["gpu-capture-frame-count"].as<uint32_t>();
            // The GPU frame capture scope only exists when enableGpuCapture is set --
            // a startup capture request implies it.
            launchOptions.enableGpuCapture |= launchOptions.captureGpuFrameOnStartup;
        }
        catch (const std::exception& e)
        {
            VKM_DEBUG_ERROR(fmt::format("Failed to parse engine launch options: {}", e.what()).c_str());
        }
        return launchOptions;
    }
}
