// Copyright (c) 2026 Snowapril

#include <vkm/renderer/engine.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph_capture.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/gpu_profiler.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/camera.h>
#include <vkm/renderer/memory_report.h>
#include <vkm/renderer/screenshot.h>
#include <vkm/platform/common/clipboard.h>
#include <vkm/base/cpu_profiler.h>
#include <vkm/base/global_variable.h>
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
#include <vkm/renderer/imgui/imgui_settings.h>
#if defined(VKM_USE_VULKAN_API)
#include <vkm/renderer/imgui/vulkan_imgui_renderer.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#elif defined(VKM_USE_METAL_API)
#include <vkm/renderer/imgui/metal_imgui_renderer.h>
#elif defined(VKM_USE_WEBGPU_API)
#include <vkm/renderer/imgui/webgpu_imgui_renderer.h>
#endif
#include <vkm/platform/common/process_stats.h>
#include <vkm/renderer/imgui/render_graph_inspector.h>
#include <vkm/renderer/imgui/memory_inspector.h>
#include <vkm/renderer/imgui/cpu_profiler_inspector.h>
#include <vkm/renderer/imgui/gpu_profiler_inspector.h>
#include <vkm/renderer/imgui/acceleration_structure_inspector.h>
#include <vkm/renderer/acceleration_structure_debug_renderer.h>
#include <imgui.h>
#endif

namespace vkm
{
    // Opens the CPU profiler window (and starts capturing) from frame 0, for profiling a
    // startup path that is over before a hotkey could be pressed: ./triangle --gv_cpu_profile=1
    VKM_GLOBAL_VARIABLE(bool, gv_cpu_profile, false);

    // Same idea for the GPU side: ./triangle --gv_gpu_profile=1 opens the GPU profiler window
    // (and starts keeping frame history) from frame 0.
    VKM_GLOBAL_VARIABLE(bool, gv_gpu_profile, false);

    // The upscale preset the engine starts in, as a VkmUpscaleMode: 0 off, 1 native AA, 2 quality,
    // 3 balanced, 4 performance. F2 cycles it at run time, so this only seeds the value --
    // ./gi --gv_upscale_mode=0 is how a run gets no temporal anti-aliasing at all.
    VKM_GLOBAL_VARIABLE(uint32_t, gv_upscale_mode, static_cast<uint32_t>(VkmUpscaleMode::Native));

    namespace
    {
        /*
        * Packing for VkmWindowContext::_pendingExtent: width in bits 32..62, height in bits
        * 0..31, and a marker in bit 63. The marker is what distinguishes "nothing pending" from
        * a genuine 0x0 publish, which is what a minimized window reports. Both dimensions are
        * pixel counts, so neither comes anywhere near the 2^31 the packing leaves them.
        */
        constexpr uint64_t kPendingExtentValidBit = uint64_t(1) << 63;

        constexpr uint64_t packPendingExtent(uint32_t width, uint32_t height)
        {
            return kPendingExtentValidBit | (static_cast<uint64_t>(width) << 32) | static_cast<uint64_t>(height);
        }

        constexpr glm::uvec2 unpackPendingExtent(uint64_t packedExtent)
        {
            return glm::uvec2(static_cast<uint32_t>((packedExtent >> 32) & 0x7FFFFFFFu),
                              static_cast<uint32_t>(packedExtent & 0xFFFFFFFFu));
        }
    }

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

        // Apply command-line global-variable overrides staged during parseEngineLaunchOptions,
        // now that the logger can report which ones matched or were rejected.
        GlobalVariableManager::singleton().applyCommandLineOverrides();

        _appDelegate.reset(appDelegate);
        _engineOptions = options;

        _renderGraphCapture = std::make_unique<VkmRenderGraphCapture>();
        if (_engineOptions.captureRenderGraphOnStartup)
        {
            _renderGraphCapture->arm();
        }
#if defined(VKM_ENABLE_IMGUI)
        _renderGraphInspector = std::make_unique<VkmRenderGraphInspector>();
        _memoryInspector = std::make_unique<VkmMemoryInspector>();
        _cpuProfilerInspector = std::make_unique<VkmCpuProfilerInspector>();
        if (gv_cpu_profile.get())
        {
            _cpuProfilerInspector->setVisible(true);
            VkmCpuProfiler::singleton().setCapturing(true);
        }

        _gpuProfilerInspector = std::make_unique<VkmGpuProfilerInspector>();
        // Visibility only: the driver is not initialized yet, so there is no VkmGpuProfiler to
        // start here (unlike the CPU profiler's process-wide singleton above). None is needed --
        // update()'s visibility-edge check below sees the window open on the first frame and
        // starts capture then.
        _gpuProfilerInspector->setVisible(gv_gpu_profile.get());
        _accelerationStructureInspector = std::make_unique<VkmAccelerationStructureInspector>();
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

        // Both halves decide whether a preset can do anything: the driver must have an upscaler,
        // and the app must render at getRenderExtent(). Without the second test a preset would
        // shrink the camera viewport under an app that keeps rendering full-screen.
        _upscaleModeAvailable =
            (_driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::TemporalUpscaling) != 0 &&
            _appDelegate->consumesUpscaleMode();
        // Chosen before postDriverReady() below, so the app sees the final value. Native AA is the
        // default wherever the upscaler exists: at ratio 1 the render extent is the display extent
        // and the upscaler is anti-aliasing rather than a performance preset.
        setUpscaleMode(static_cast<VkmUpscaleMode>(
            std::min(gv_upscale_mode.get(), static_cast<uint32_t>(VkmUpscaleMode::Count) - 1u)));

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

#if defined(VKM_ENABLE_IMGUI)
        // Here rather than beside the inspector in initialize(): the pipeline it resolves only
        // exists once the directory above has loaded. Skipped where no acceleration structure can
        // exist, so a device without ray tracing allocates nothing for an always-empty view. A
        // failure leaves the pointer null and is logged -- a debug overlay must not stop startup.
        if ((_driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::RayTracing) != 0)
        {
            auto debugRenderer = std::make_unique<VkmAccelerationStructureDebugRenderer>();
            std::string debugRendererError;
            if (debugRenderer->initialize(_driver, _pipelineStateManager.get(), &debugRendererError))
            {
                _asDebugRenderer = std::move(debugRenderer);
            }
            else
            {
                VKM_DEBUG_ERROR(fmt::format("Failed to initialize the acceleration structure debug "
                                            "renderer: {}", debugRendererError).c_str());
            }
        }
#endif

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

    void VkmEngine::setUpscaleMode(const VkmUpscaleMode mode)
    {
        const VkmUpscaleMode resolved = _upscaleModeAvailable ? mode : VkmUpscaleMode::Off;
        if (resolved == _upscaleMode)
        {
            return;
        }
        _upscaleMode = resolved;
        // Logged because a change is a visible hitch: every consumer rebuilds its
        // render-extent-sized targets, its resource tables and its upscaler.
        VKM_DEBUG_INFO(fmt::format("Upscale mode: {} ({:.2f}x render scale)",
                                   vkmUpscaleModeName(_upscaleMode),
                                   vkmUpscaleModeScale(_upscaleMode))
                           .c_str());
    }

    glm::uvec2 VkmEngine::getRenderExtent()
    {
        VkmSwapChainBase* swapChain = getMainSwapChain();
        // Called before the first addSwapChain() during startup, and while a window is minimized.
        const glm::uvec2 displayExtent = swapChain != nullptr ? swapChain->getExtent() : glm::uvec2(0u);
        if (displayExtent.x == 0 || displayExtent.y == 0)
        {
            return displayExtent;
        }
        return vkmUpscaleRenderExtent(displayExtent, _upscaleMode);
    }

    void VkmEngine::loopInner(const double currentUpdateTime)
    {
        // Closes the previous frame's profile before anything of this frame is recorded, so a
        // collected frame lines up with exactly one loopInner() call. No-op while not capturing.
        VkmCpuProfiler::singleton().beginFrame();
        VKM_PROFILE_SCOPE("Frame");

        // Retires whichever GPU submissions have finished since the last frame. Deliberately
        // right after the CPU profiler's frame boundary, so both collectors advance together.
        _driver->getGpuProfiler()->collect();

        const double deltaTime = currentUpdateTime - _lastUpdateTime;
        _lastUpdateTime = currentUpdateTime;

#if defined(VKM_GPU_CAPTURE)
        // Frame-boundary driver hooks (MTLCaptureScope begin/end on Metal) bracket all of
        // this frame's encoding, submission, and present.
        _driver->onFrameBegin();
#endif // VKM_GPU_CAPTURE

        // Drains events pushed from platform callback threads and clears the previous frame's
        // edge/delta state before any update()/render() code queries input.
        {
            VKM_PROFILE_SCOPE("Input::beginFrame");
            _inputHandler.beginFrame();
        }

#if defined(VKM_ENABLE_IMGUI)
        if (_imGuiRenderer)
        {
            VKM_PROFILE_SCOPE("ImGui::newFrame");
            // Backends that poll the mouse themselves must know whether the ImGui window is the
            // one being typed into; the engine is the only place that knows.
            const bool imGuiWindowFocused = (_imGuiWindowIndex != INVALID_VALUE32) &&
                                            (_inputHandler.getFocusedWindowIndex() == _imGuiWindowIndex);
            _imGuiRenderer->newFrame(imGuiWindowFocused);
        }
#endif

        update( deltaTime );
        render( deltaTime );

#if defined(VKM_GPU_CAPTURE)
        _driver->onFrameEnd();
#endif // VKM_GPU_CAPTURE

        _currentFrameIndex = (_currentFrameIndex + 1) % FRAME_COUNT;
        // Monotonic, unlike the slot index above. Wrapping after 2^32 frames is fine: it only
        // ever seeds hashes, and at 60 fps that is over two years of continuous running.
        ++_frameCounter;
    }

    uint32_t VkmEngine::findWindowIndex(const void* nativeHandle) const
    {
        if (nativeHandle == nullptr)
        {
            return kVkmNoFocusedWindow;
        }

        for (uint32_t windowIndex = 0; windowIndex < _windowContexts.size(); ++windowIndex)
        {
            if (_windowContexts[windowIndex]._windowHandle == nativeHandle)
            {
                return windowIndex;
            }
        }
        return kVkmNoFocusedWindow;
    }

    void VkmEngine::onWindowFocusChanged(const void* nativeHandle, bool focused)
    {
        const uint32_t windowIndex = findWindowIndex(nativeHandle);
        if (windowIndex == kVkmNoFocusedWindow)
        {
            // A window this engine does not own (or a focus notification arriving before
            // addSwapChain registered it) says nothing about our windows.
            return;
        }
        _inputHandler.onWindowFocusChanged(windowIndex, focused);
    }

    void VkmEngine::onWindowResized(const void* nativeHandle, uint32_t width, uint32_t height)
    {
        const uint32_t windowIndex = findWindowIndex(nativeHandle);
        if (windowIndex == kVkmNoFocusedWindow)
        {
            // A window this engine does not own, or an event arriving before addSwapChain()
            // registered it -- same rule as onWindowFocusChanged().
            return;
        }

        // Overwrite rather than accumulate: only the newest size matters, and a resize drag
        // produces far more events than frames.
        _windowContexts[windowIndex]._pendingExtent.store(packPendingExtent(width, height), std::memory_order_release);

        // Input state that a geometry change invalidates is the input handler's business; it
        // needs the event ordered against the cursor moves around it, so it goes on that queue.
        _inputHandler.onWindowResized(windowIndex, width, height);
    }

    void VkmEngine::onWindowLiveResizeChanged(const void* nativeHandle, bool active)
    {
        const uint32_t windowIndex = findWindowIndex(nativeHandle);
        if (windowIndex == kVkmNoFocusedWindow)
        {
            return;
        }
        _windowContexts[windowIndex]._liveResizeActive.store(active, std::memory_order_release);
    }

    bool VkmEngine::isWindowRenderingSuspended(uint32_t windowIndex) const
    {
        if (windowIndex >= _windowContexts.size())
        {
            return true;
        }

        const VkmWindowContext& windowContext = _windowContexts[windowIndex];
        if (windowContext._liveResizeActive.load(std::memory_order_acquire))
        {
            return true;
        }

        const glm::uvec2 extent = windowContext._swapChain->getExtent();
        return extent.x == 0 || extent.y == 0;
    }

    void VkmEngine::recreateSwapChain(VkmWindowContext& windowContext, uint64_t packedExtent)
    {
        VkmSwapChainBase* swapChain = windowContext._swapChain;
        const glm::uvec2 currentExtent = swapChain->getExtent();
        // No published size means this is an out-of-date recreate: rebuild at the extent the
        // swapchain already has, which the backend re-derives from its surface anyway.
        const glm::uvec2 targetExtent = (packedExtent != 0) ? unpackPendingExtent(packedExtent) : currentExtent;

        // Plenty of published sizes change nothing: the platform layer re-reporting the size the
        // swapchain was created at, or a drag that ended where it began. resize() would ignore
        // those anyway, but only after this function had already paid for the drain below --
        // so the same condition is checked here, before anything expensive happens.
        if (targetExtent == currentExtent && swapChain->isOutOfDate() == false)
        {
            return;
        }

        // Every frame slot of this window may still have a submit in flight referencing the back
        // buffers, and the driver's other queues (uploads, readbacks) may hold work of their own.
        // Drain both before the swapchain releases those textures.
        for (std::unique_ptr<VkmRenderGraph>& renderGraph : windowContext._frameRenderGraphs)
        {
            renderGraph->ensureCompleted();
        }
        _driver->waitIdle();

        swapChain->resize(targetExtent.x, targetExtent.y);

        VKM_DEBUG_INFO(fmt::format("Swapchain recreated at {}x{}",
            swapChain->getExtent().x, swapChain->getExtent().y).c_str());
    }

    bool VkmEngine::wantsCaptureKeyboard() const
    {
#if defined(VKM_ENABLE_IMGUI)
        // Platform callbacks can fire before addSwapChain() has created the ImGui context,
        // and ImGui::GetIO() asserts when no context exists.
        if (ImGui::GetCurrentContext() == nullptr)
        {
            return false;
        }

        return ImGui::GetIO().WantCaptureKeyboard;
#else
        return false;
#endif
    }

    bool VkmEngine::wantsCaptureMouse() const
    {
#if defined(VKM_ENABLE_IMGUI)
        if (ImGui::GetCurrentContext() == nullptr)
        {
            return false;
        }

        return ImGui::GetIO().WantCaptureMouse;
#else
        return false;
#endif
    }

    void VkmEngine::destroy()
    {
        // Before anything is torn down, while the driver and its resource pool can still
        // report what is live -- a leak shows up here as resources that never went away.
        logMemoryReport(captureMemorySnapshot(_driver));

        if (_renderGraphCapture)
        {
            _renderGraphCapture->releaseResources(_driver);
        }
#if defined(VKM_ENABLE_IMGUI)
        if (_renderGraphInspector)
        {
            _renderGraphInspector->releaseResources(_driver);
        }
        if (_asDebugRenderer)
        {
            _asDebugRenderer->releaseResources();
            _asDebugRenderer.reset();
        }
#endif
#if defined(VKM_ENABLE_IMGUI)
        if (_imGuiRenderer)
        {
            _imGuiRenderer->shutdown();
            _imGuiRenderer.reset();
        }
#endif

        for (VkmWindowContext& windowContext : _windowContexts)
        {
            // The per-frame render graphs must not outlive the swapchain they present to;
            // release them first so no in-flight submit still references it.
            for (std::unique_ptr<VkmRenderGraph>& renderGraph : windowContext._frameRenderGraphs)
            {
                renderGraph.reset();
            }
            delete windowContext._swapChain;
            windowContext._swapChain = nullptr;
        }
        _windowContexts.clear();
        _imGuiWindowIndex = INVALID_VALUE32;
    }

    uint32_t VkmEngine::addSwapChain(const VkmWindowInfo& windowInfo, bool isImGuiWindow)
    {
        VkmSwapChainBase* swapChain = _driver->newSwapChain();
        const bool result = swapChain->initialize(windowInfo);
        VKM_ASSERT(result, "Failed to create swapchain");

        // The swapchain color format is engine-decided once at driver init (the single source of
        // truth used for swapchain creation and "swapchain" pipeline-format resolution alike), so
        // read it rather than re-deriving it per backend from the swapchain object.
        const VkmFormat backBufferFormat = _driver->getSwapChainColorFormat();

        const uint32_t windowIndex = static_cast<uint32_t>(_windowContexts.size());

        // Constructed in place: VkmWindowContext holds atomics, so it cannot be moved in.
        VkmWindowContext& windowContext = _windowContexts.emplace_back();
        windowContext._swapChain = swapChain;
        windowContext._windowHandle = windowInfo._windowHandle;
        windowContext._backBufferFormat = backBufferFormat;
        windowContext._isImGuiWindow = isImGuiWindow;
        for (uint8_t i = 0; i < FRAME_COUNT; ++i)
        {
            windowContext._frameRenderGraphs[i] = std::make_unique<VkmRenderGraph>(_driver, i);
        }

#if defined(VKM_ENABLE_IMGUI)
        if (isImGuiWindow)
        {
            VKM_ASSERT(_imGuiRenderer == nullptr, "ImGui window already exists");
#if defined(VKM_USE_VULKAN_API)
            _imGuiRenderer = std::make_unique<VkmImGuiRendererVulkan>(_driver);
#elif defined(VKM_USE_METAL_API)
            _imGuiRenderer = std::make_unique<VkmImGuiRendererMetal>(_driver);
#elif defined(VKM_USE_WEBGPU_API)
            _imGuiRenderer = std::make_unique<VkmImGuiRendererWebGPU>(_driver);
#endif
            const bool imGuiInitialized = _imGuiRenderer->initialize(windowInfo._windowHandle, backBufferFormat);
            VKM_ASSERT(imGuiInitialized, "Failed to initialize ImGui renderer");
            // The context exists and no frame has opened yet, which is what the font atlas and
            // style the cached settings touch require.
            vkmLoadImGuiSettings();
            _imGuiWindowIndex = windowIndex;
        }
#else
        (void)isImGuiWindow;
#endif

        return windowIndex;
    }

    void VkmEngine::update(const double deltaTime)
    {
        VKM_PROFILE_SCOPE("Engine::update");
#if defined(VKM_ENABLE_IMGUI)
        if (_imGuiRenderer)
        {
            // Must run before the frame's first ImGui::Render() call (triggered lazily by
            // VkmImGuiRendererBase::renderDrawData() in render() below) -- ImGui::Begin/End
            // calls made after that point in the same frame would be dropped.
            // Sampled before the overlay so both windows show the same numbers this frame.
            {
                VKM_PROFILE_SCOPE("MemoryInspector::update");
                _memoryInspector->update(_driver, deltaTime);
            }
            // Same cadence: flags PSO json/shader edits (and auto-reloads them when enabled)
            // at a quiescent point, before render() records anything this frame.
            _pipelineStateManager->pollSourceChanges(deltaTime);
            renderDebugOverlay(deltaTime);

            // Gated so a driver with no upscaler, or an app that does not consume the mode, never
            // cycles a setting with no visible effect. No auto-repeat: each step rebuilds every
            // render-extent target and recreates the upscaler.
            if (_upscaleModeAvailable && ImGui::IsKeyPressed(ImGuiKey_F2, false))
            {
                setUpscaleMode(vkmNextUpscaleMode(_upscaleMode));
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F4, false))
            {
                _accelerationStructureInspector->toggleVisible();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
            {
                _renderGraphInspector->toggleVisible();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F6, false))
            {
                _gpuProfilerInspector->toggleVisible();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F7, false))
            {
                _cpuProfilerInspector->toggleVisible();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F8, false))
            {
                _memoryInspector->toggleVisible();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F3, false))
            {
                _clipboardCaptureArmed = true;
            }
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
            {
                VKM_PROFILE_SCOPE("Inspectors::draw");
                _renderGraphInspector->draw(*_renderGraphCapture, _driver, _imGuiRenderer.get(),
                                            _pipelineStateManager.get(), _driver->getGpuProfiler());
                _memoryInspector->draw();
                _cpuProfilerInspector->draw();
                _gpuProfilerInspector->draw(_driver->getGpuProfiler());
                _accelerationStructureInspector->draw(_driver);
            }

            // Outside the draw above, so the overlay follows the toggle even while the window is
            // closed -- draw() early-returns on !_visible without touching either value.
            if (_asDebugRenderer)
            {
                _asDebugRenderer->setEnabled(_accelerationStructureInspector->isSceneOverlayEnabled());
                _asDebugRenderer->setSelected(_accelerationStructureInspector->getSelected());
            }

            // Collection follows the window: closing it (with F7 or the title bar's close
            // button) stops recording, so nothing is measured while nothing is looking. The
            // inspector's own Start/Stop button can decouple the two within a session, which is
            // why this only acts on a change of visibility rather than asserting every frame.
            if (_cpuProfilerInspector->isVisible() != _cpuProfilerWasVisible)
            {
                _cpuProfilerWasVisible = _cpuProfilerInspector->isVisible();
                VkmCpuProfiler::singleton().setCapturing(_cpuProfilerWasVisible);
            }

            // Same follows-the-window rule, but it only gates the frame *history*: the GPU
            // profiler keeps recording timestamps either way, because the overlay's GPU stat
            // below reads the same collector whether or not this window is open.
            if (_gpuProfilerInspector->isVisible() != _gpuProfilerWasVisible)
            {
                _gpuProfilerWasVisible = _gpuProfilerInspector->isVisible();
                _driver->getGpuProfiler()->setCapturing(_gpuProfilerWasVisible);
            }
        }
#endif
        {
            VKM_PROFILE_SCOPE("App::update");
            _appDelegate->update(deltaTime);
        }
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
        // Backend-free: VkmGpuProfiler is where every backend's timestamps land now, so this
        // no longer needs to know which driver it is talking to. Reports 0 on a device without
        // timestamp query support.
        ImGui::Text("GPU: %.2f ms", _driver->getGpuProfiler()->getLastFrameGpuTimeMs());
        ImGui::Text("Frame: %u", _currentFrameIndex);

        // Read from the inspector's cached sample rather than re-querying: the tag table
        // lives behind the allocator's global mutex.
        const VkmMemorySnapshot& memory = _memoryInspector->getSnapshot();
        if (memory._process._valid)
        {
            ImGui::Text("Mem: %s (tracked %s)", formatByteSize(memory._process._residentBytes).c_str(),
                        formatByteSize(memory._cpuTrackedUsableBytes).c_str());
        }
        if (memory._gpu._hasDeviceStats)
        {
            ImGui::Text("VRAM: %s / %s", formatByteSize(memory._gpu._deviceAllocatedBytes).c_str(),
                        formatByteSize(memory._gpu._deviceBudgetBytes).c_str());
        }

        if (_upscaleModeAvailable)
        {
            ImGui::Text("F2: upscale mode (%s)", vkmUpscaleModeName(_upscaleMode));
        }
#if (defined(VKM_PLATFORM_APPLE) && defined(VKM_USE_METAL_API)) || (defined(VKM_PLATFORM_WINDOWS) && defined(VKM_USE_VULKAN_API))
        ImGui::Text("F3: copy back buffer to clipboard");
#endif
        ImGui::Text("F4: acceleration structures");
        ImGui::Text("F5: render graph inspector");
        ImGui::Text("F6: GPU profiler");
        ImGui::Text("F7: CPU profiler");
        ImGui::Text("F8: memory inspector");
        ImGui::Text("F10: capture render graph");
#if defined(VKM_USE_METAL_API) && defined(VKM_GPU_CAPTURE)
        ImGui::Text("F9: capture GPU frame (.gputrace)");
#endif

        ImGui::Separator();
        vkmDrawImGuiFontScaleSlider();
        ImGui::End();
    }
#endif

    void VkmEngine::captureBackBufferToClipboard(VkmResourceHandle backBuffer, VkmFormat format)
    {
#if (defined(VKM_PLATFORM_APPLE) && defined(VKM_USE_METAL_API)) || (defined(VKM_PLATFORM_WINDOWS) && defined(VKM_USE_VULKAN_API))
        // The back buffer is only created as a copy source when it was asked for at launch, since
        // making it one costs the driver's framebuffer-only fast paths every frame.
        if (!_engineOptions.enableBackBufferReadback)
        {
            VKM_DEBUG_WARN("Clipboard capture needs a readable back buffer; relaunch with --enable-backbuffer-readback");
            return;
        }
#if defined(VKM_USE_VULKAN_API)
        // Surfaces that cannot supply a transfer-source back buffer leave the flag unset; copying
        // from one anyway would be invalid.
        VkmTexture* texture = _driver->getRenderResourcePool()->getResource<VkmTexture>(backBuffer);
        if (texture == nullptr ||
            (texture->getTextureInfo()._flags & VkmResourceCreateInfo::AllowTransferSrc) == 0)
        {
            VKM_DEBUG_ERROR("Clipboard capture: this surface's back buffer is not a transfer source");
            return;
        }
#endif
        const VkmTextureReadbackResult readback = _driver->readbackTexture(backBuffer);
        std::vector<uint8_t> rgba8;
        if (!vkmConvertReadbackToRgba8(readback, format, rgba8))
        {
            return; // vkmConvertReadbackToRgba8 logged the reason
        }
        if (vkmSetClipboardImage(readback.width, readback.height, rgba8.data()))
        {
            VKM_DEBUG_INFO("Copied back buffer to clipboard");
        }
        else
        {
            VKM_DEBUG_ERROR("Clipboard capture: failed to set the OS clipboard");
        }
#else
        (void)backBuffer;
        (void)format;
        VKM_DEBUG_WARN("Clipboard capture is not supported on this platform and backend");
#endif
    }

    void VkmEngine::render(const double deltaTime)
    {
        VKM_PROFILE_SCOPE("Engine::render");
        (void)deltaTime;

        // Each window is driven independently: its own frame-slot render graph is throttled and
        // reset, its own back buffer acquired, its own submit carries only its own presentSwapChain,
        // and it presents on its own. Running one execute()/submit per window (rather than one per
        // frame) is what keeps the backend's "exactly one presentSwapChain per submit" invariant
        // valid with multiple windows.
        const bool soleWindow = (_windowContexts.size() == 1);

        for (uint32_t windowIndex = 0; windowIndex < _windowContexts.size(); ++windowIndex)
        {
            VkmWindowContext& windowContext = _windowContexts[windowIndex];
            VkmRenderGraph* renderGraph = windowContext._frameRenderGraphs[_currentFrameIndex].get();

            // Rendering stops for the whole duration of a resize drag. The last presented frame
            // stays on screen, scaled into the new bounds by the compositor, until the drag ends.
            if (windowContext._liveResizeActive.load(std::memory_order_acquire))
            {
                continue;
            }

            // A resize ended (or the swapchain went out of date under us): drain and rebuild
            // before anything acquires from it. Consuming with exchange() means a size published
            // while we rebuild is picked up next frame rather than lost.
            const uint64_t pendingExtent = windowContext._pendingExtent.exchange(0, std::memory_order_acq_rel);
            if (pendingExtent != 0 || windowContext._swapChain->isOutOfDate())
            {
                recreateSwapChain(windowContext, pendingExtent);
            }

            // A minimized window has no swapchain to render into.
            const glm::uvec2 windowExtent = windowContext._swapChain->getExtent();
            if (windowExtent.x == 0 || windowExtent.y == 0)
            {
                continue;
            }

            // Throttle before acquiring: timeline-wait on this window's previous submit on this
            // frame slot and reset its graph. Must precede acquireNextImage() so this slot's
            // image-available semaphore is guaranteed free before it is reused.
            {
                VKM_PROFILE_SCOPE("RenderGraph::ensureCompleted");
                renderGraph->ensureCompleted();
            }
            renderGraph->reset();

            // Rewind this frame slot's push-constant ring region, for the same reason and at the
            // same point as the set-1 write below: the ring's per-slot region may only be
            // rewritten once that slot's previous submit has completed. Driven by window 0 only,
            // because the ring is engine-global per slot -- the same single-region caveat set 1
            // carries (see TODO.md). Nothing else pushes: a dedicated ImGui window renders through
            // the ImGui renderer's own argument table, not this ring.
            if (windowIndex == 0)
            {
                _driver->getBindlessResourceManager()->beginFrame(_currentFrameIndex);
            }

            VkmResourceHandle currentBackBuffer = VKM_INVALID_RESOURCE_HANDLE;
            {
                VKM_PROFILE_SCOPE("SwapChain::acquireNextImage");
                currentBackBuffer = windowContext._swapChain->acquireNextImage();
            }
            if (!currentBackBuffer.isValid())
            {
                // Acquire failed. Only this window's slot was waited on and reset, so skip just
                // this window this frame. An out-of-date surface flagged itself on the way out
                // and is rebuilt at the top of the next frame; anything else is a real error the
                // backend has already logged.
                continue;
            }

            // Publish this frame slot's camera constants into descriptor set 1. Placed here
            // because the write is a plain host write with no GPU synchronization: it has to
            // follow this slot's ensureCompleted() above, and it has to follow acquire so a
            // resize's new extent is the one the projection sees. There is a single set-1
            // region per frame slot engine-wide, so only the primary window drives it -- see
            // VkmEngine::setActiveCamera.
            if (windowIndex == 0)
            {
                VkmFrameConstants frameConstants{}; // identity while no camera is registered
                if (_activeCamera != nullptr)
                {
                    // The upscale mode owns the render extent: below the display extent, the
                    // camera's aspect and set 1's _viewportSize describe the render extent while
                    // the swapchain keeps its own. Off and Native both leave them equal.
                    const glm::uvec2 cameraExtent =
                        vkmUpscaleRenderExtent(windowContext._swapChain->getExtent(), _upscaleMode);
                    _activeCamera->setViewportSize(cameraExtent.x, cameraExtent.y);
                    _activeCamera->fillFrameConstants(frameConstants);

                    // The camera fills everything derivable from this frame alone; the
                    // frame-to-frame fields are the engine's. Seeding _prevViewProjection with
                    // the current matrix on the first frame makes reprojection the identity
                    // there, instead of reprojecting against the identity matrix and reading as
                    // a violent camera cut. Both sides of the reprojection pair are jitter-free,
                    // so motion vectors never carry the sub-pixel jitter a temporal upscaler
                    // receives separately through _jitter.
                    if (_hasPrevViewProjection)
                    {
                        frameConstants._prevViewProjection = _prevViewProjection;
                        frameConstants._prevCameraPositionWorld = _prevCameraPositionWorld;
                        frameConstants._jitter.z = _prevJitter.x;
                        frameConstants._jitter.w = _prevJitter.y;
                    }
                    else
                    {
                        frameConstants._prevViewProjection = frameConstants._viewProjectionNoJitter;
                        frameConstants._prevCameraPositionWorld = frameConstants._cameraPositionWorld;
                    }
                    _prevViewProjection = frameConstants._viewProjectionNoJitter;
                    _prevCameraPositionWorld = frameConstants._cameraPositionWorld;
                    _prevJitter = glm::vec2(frameConstants._jitter.x, frameConstants._jitter.y);
                    _hasPrevViewProjection = true;
                }
                else
                {
                    // No camera: nothing sensible to reproject against next frame either.
                    _hasPrevViewProjection = false;
                }
                frameConstants._frameIndex = glm::uvec4(_frameCounter, 0u, 0u, 0u);
                _driver->getFrameConstantManager()->update(_currentFrameIndex, frameConstants);
            }

            // A dedicated ImGui window renders ImGui only; any other window renders the app scene.
            // In single-window mode the sole (ImGui) window renders the scene first, then the
            // ImGui overlay on top of it.
            const bool appRendersHere = !windowContext._isImGuiWindow || soleWindow;
            if (appRendersHere)
            {
                VKM_PROFILE_SCOPE("App::render");
                _appDelegate->render(windowIndex, renderGraph, currentBackBuffer);
            }

#if defined(VKM_ENABLE_IMGUI)
            // Between the app's passes and the ImGui overlay: over the scene, under the UI.
            // Window 0 only, because set 1 carries the primary window's camera and these boxes
            // are in that camera's world. The swapchain extent, not the render extent: this draws
            // into the back buffer, which an upscaler has already resolved to display size.
            if (windowIndex == 0 && appRendersHere && _asDebugRenderer)
            {
                VKM_PROFILE_SCOPE("AsDebugRenderer::record");
                _asDebugRenderer->record(renderGraph, currentBackBuffer, windowExtent, _currentFrameIndex);
            }

            if (windowContext._isImGuiWindow && _imGuiRenderer)
            {
                // If the app already recorded into this back buffer (single-window mode), load it;
                // a dedicated ImGui window clears instead, since nothing else draws to it.
                VkmFrameBufferDescriptor imGuiFrameBufferDesc;
                imGuiFrameBufferDesc._renderPass._colorAttachmentCount = 1;
                imGuiFrameBufferDesc._renderPass._colorAttachments[0]._attachmentId = 0;
                imGuiFrameBufferDesc._renderPass._colorAttachments[0]._loadAction =
                    appRendersHere ? VkmLoadAction::Load : VkmLoadAction::Clear;
                imGuiFrameBufferDesc._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
                imGuiFrameBufferDesc._renderPass._colorAttachments[0]._clearColors[0] = 0.1f;
                imGuiFrameBufferDesc._renderPass._colorAttachments[0]._clearColors[1] = 0.1f;
                imGuiFrameBufferDesc._renderPass._colorAttachments[0]._clearColors[2] = 0.1f;
                imGuiFrameBufferDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;
                imGuiFrameBufferDesc._width = windowContext._swapChain->getExtent().x;
                imGuiFrameBufferDesc._height = windowContext._swapChain->getExtent().y;
                imGuiFrameBufferDesc._colorAttachments[0] = currentBackBuffer;

                VkmRenderGraphicsSubGraph* imGuiSubGraph = renderGraph->beginGraphicsSubGraph(imGuiFrameBufferDesc, "EngineImGuiOverlay");
                VkmImGuiRendererBase* imGuiRenderer = _imGuiRenderer.get();
                imGuiSubGraph->setRenderCallback([imGuiRenderer](VkmCommandBufferBase* commandBuffer) {
                    VKM_PROFILE_SCOPE("ImGui::renderDrawData");
                    imGuiRenderer->renderDrawData(commandBuffer);
                });

                // While a capture is inspectable, the ImGui pass may sample its snapshot textures
                // (owned engine-globally by the capture, not by any one swapchain) -- reference
                // them so recordUsage() tracks the in-flight draws and the deferred reclaimer waits
                // for them when the capture is released.
                if (_renderGraphCapture->getState() == VkmRenderGraphCapture::State::Ready)
                {
                    for (VkmResourceHandle snapshotHandle : _renderGraphCapture->getSnapshotTextureHandles())
                    {
                        imGuiSubGraph->addReferencedResource(snapshotHandle, VkmResourceAccess::ShaderSampledRead);
                    }
                }
                // Same reasoning for the textures the inspector previewed directly out of the
                // resource pool (the texture browser, and live input previews in a capture).
                for (VkmResourceHandle sampledHandle : _renderGraphInspector->getTexturesSampledLastDraw())
                {
                    imGuiSubGraph->addReferencedResource(sampledHandle, VkmResourceAccess::ShaderSampledRead);
                }
            }
#endif

            {
                VKM_PROFILE_SCOPE("RenderGraph::compile");
                renderGraph->compile();
            }

            // Capture the primary (window 0) render graph so the inspector shows the app's scene
            // passes; the inspector UI itself is drawn on the ImGui window and samples the
            // resulting snapshot textures across windows.
            VkmRenderGraphCapture* capture =
                (windowIndex == 0 && _renderGraphCapture->getState() == VkmRenderGraphCapture::State::Armed)
                    ? _renderGraphCapture.get() : nullptr;
            renderGraph->execute(VkmRenderGraphCommitOptions{ .waitForCompletion = false, .presentSwapChain = windowContext._swapChain, .capture = capture });
            if (capture != nullptr)
            {
                // One deliberate hitch on the capture frame: the buffer readbacks and snapshot
                // copies must have completed on the GPU before finalize() maps them.
                renderGraph->ensureCompleted();
                capture->finalize(_driver);
            }

            if (windowIndex == 0 && _clipboardCaptureArmed)
            {
                // One deliberate hitch, as on a capture frame: the back buffer's last write must
                // have completed before the blocking readback maps it.
                _clipboardCaptureArmed = false;
                renderGraph->ensureCompleted();
                captureBackBufferToClipboard(currentBackBuffer, windowContext._backBufferFormat);
            }

            {
                VKM_PROFILE_SCOPE("SwapChain::present");
                windowContext._swapChain->present();
            }
        }

#if defined(VKM_ENABLE_IMGUI)
        // loopInner() opened an ImGui frame unconditionally, but every path above that skips the
        // ImGui window -- suspended during a live resize, minimized, or a failed acquire -- left
        // it open. Closing it here keeps the next ImGui::NewFrame() from asserting. A no-op on
        // the ordinary path, where the overlay's render callback already ended the frame.
        if (_imGuiRenderer)
        {
            _imGuiRenderer->discardFrame();
        }
#endif
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
                cxxopts::value<uint32_t>()->default_value(std::to_string(DEFAULT_ENGINE_LAUNCH_OPTIONS.gpuCaptureFrameCount)))
            ("enable-hdr", "Request an HDR swapchain (used only if the display supports it)",
                cxxopts::value<bool>()->default_value(DEFAULT_ENGINE_LAUNCH_OPTIONS.enableHdr ? "true" : "false"))
            ("enable-backbuffer-readback", "Make the back buffer readable so F2 can copy the frame to the clipboard (gives up the driver's framebuffer-only fast paths)",
                cxxopts::value<bool>()->default_value(DEFAULT_ENGINE_LAUNCH_OPTIONS.enableBackBufferReadback ? "true" : "false"));

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
            launchOptions.enableHdr = result["enable-hdr"].as<bool>();
            launchOptions.enableBackBufferReadback = result["enable-backbuffer-readback"].as<bool>();
            // The GPU frame capture scope only exists when enableGpuCapture is set --
            // a startup capture request implies it.
            launchOptions.enableGpuCapture |= launchOptions.captureGpuFrameOnStartup;

            // Anything the engine did not recognize is offered to the global-variable manager
            // as a "--<name>=<value>" override. Only stage here (the logger is not up yet);
            // applyCommandLineOverrides() runs in initializeEngine once it can report matches.
            GlobalVariableManager::singleton().setCommandLineOverrides(result.unmatched());
        }
        catch (const std::exception& e)
        {
            VKM_DEBUG_ERROR(fmt::format("Failed to parse engine launch options: {}", e.what()).c_str());
        }
        return launchOptions;
    }
}
