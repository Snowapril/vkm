// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/platform/common/app_delegate.h>
#include <vkm/platform/common/input_handler.h>
#include <vkm/platform/common/window.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/upscaler.h>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <atomic>
#include <deque>
#include <memory>

namespace vkm
{
    class VkmDriverBase;
    class VkmTexture;
    class VkmSwapChainBase;
    class VkmPipelineStateManager;
    class VkmRenderGraphCapture;
    class VkmCamera;
#if defined(VKM_ENABLE_IMGUI)
    class VkmImGuiRendererBase;
    class VkmRenderGraphInspector;
    class VkmMemoryInspector;
    class VkmCpuProfilerInspector;
    class VkmGpuProfilerInspector;
    class VkmAccelerationStructureInspector;
    class VkmAccelerationStructureDebugRenderer;
#endif
    struct VkmInitResult;

    /*
    * @brief One window owned by the engine: its swapchain, native handle, and per-frame-slot
    * render graphs. Each window is driven independently in VkmEngine::render() -- own acquire,
    * own submit carrying its own presentSwapChain, own present -- which is what keeps the
    * backend's "exactly one presentSwapChain per submit" invariant intact with N windows.
    */
    struct VkmWindowContext
    {
        VkmSwapChainBase* _swapChain = nullptr;
        void* _windowHandle = nullptr; // native handle (GLFWwindow* / CAMetalLayer*)
        VkmFormat _backBufferFormat = VkmFormat::Undefined;
        bool _isImGuiWindow = false; // the single window ImGui is bound to and drawn on
        std::array<std::unique_ptr<VkmRenderGraph>, FRAME_COUNT> _frameRenderGraphs;

        /*
        * @brief Resize handoff from the window thread to the engine loop.
        * @details Resize events arrive on the platform's window thread, which on the macOS Metal
        * path is not the thread running loopInner(). Rather than let that thread touch the driver,
        * it only publishes here, and VkmEngine::render() consumes it and does every GPU call. Same
        * split as VkmInputHandler's event queue, with atomics instead of a mutex, there being one
        * word of state per direction.
        * _pendingExtent is a packed extent plus a marker bit, or 0 for no pending change; the
        * marker keeps a genuine 0x0, a minimized window, from reading as nothing pending. See
        * packPendingExtent() in engine.cpp for the layout. Publishing overwrites rather than
        * queues, only the newest size mattering.
        */
        std::atomic<uint64_t> _pendingExtent {0};
        // Set between a live-resize begin and end (AppKit's windowWillStartLiveResize /
        // windowDidEndLiveResize). While set, this window renders nothing at all.
        std::atomic<bool> _liveResizeActive {false};
    };

    struct VkmEngineLaunchOptions
    {
        bool enableValidationLayer;
        bool enableGpuCapture = false;
        bool enableGpuCrashDump = false;
        // Arm a render graph capture at startup so the first rendered frame is captured
        // (equivalent to pressing the capture hotkey before frame 0).
        bool captureRenderGraphOnStartup = false;
        // Capture a .gputrace at startup (Metal only; implies enableGpuCapture).
        // Equivalent to pressing F9 before frame 0. The capture starts
        // gpuCaptureStartFrame frames later and spans gpuCaptureFrameCount consecutive
        // frames (these two also apply to F9-triggered captures).
        bool captureGpuFrameOnStartup = false;
        uint32_t gpuCaptureStartFrame = 0;
        uint32_t gpuCaptureFrameCount = 1;
        // Request an HDR swapchain. The engine still only uses an HDR format when the display
        // actually supports it (see VkmDriverBase::selectSwapChainColorFormat); otherwise it
        // falls back to the non-HDR format. Off by default -- HDR is opt-in.
        bool enableHdr = false;
        // Create the back buffer so it can be read back, which is what the F3 clipboard capture
        // needs. Off by default: a readable back buffer gives up the driver's framebuffer-only
        // fast paths (lossless compression, direct-to-display), and a run that never captures
        // should not pay for them.
        bool enableBackBufferReadback = false;
    };
    constexpr const VkmEngineLaunchOptions DEFAULT_ENGINE_LAUNCH_OPTIONS = { true, false, false, false, false, 0, 1, false, false };

    /*
    * @brief Engine base class
    * @details manage whole engine lifecycle and drive the render driver and other modules
    */
    class VkmEngine
    {
    public:
        VkmEngine(VkmDriverBase* driver);
        ~VkmEngine();

        /*
        * @brief Initialize engine
        * @details initialize logger manager and other modules
        */
        bool initializeEngine(AppDelegate* appDelegate, VkmEngineLaunchOptions options = DEFAULT_ENGINE_LAUNCH_OPTIONS);

        /*
        * @brief Initialize backend driver
        */
        VkmInitResult initializeBackendDriver();

        /*
        * @brief Run engine loop
        * @details run main loop of engine
        */
        void loopInner(const double currentUpdateTime);

        /*
        * @brief Destroy engine
        * @details destroy all modules and logger manager
        */
        void destroy();

        /*
        * @brief Add a window (with its own swapchain) to the engine. Returns the window index
        * used to identify it later (e.g. in AppDelegate::render). When isImGuiWindow is true,
        * the engine's single ImGui renderer is bound to this window and the ImGui overlay is
        * drawn here; exactly one window should be created with isImGuiWindow = true.
        */
        uint32_t addSwapChain(const VkmWindowInfo& windowInfo, bool isImGuiWindow = false);

    private:
        /*
         
         */
        void update(const double deltaTime);

        /*

         */
        void render(const double deltaTime);

        /*
        * @brief Drains everything that could still reference `windowContext`'s back buffers, then
        * rebuilds its swapchain at `packedExtent` (see packPendingExtent() in engine.cpp), or at
        * the swapchain's current extent when `packedExtent` is 0 -- an out-of-date recreate,
        * where the backend re-derives the size from its surface anyway.
        * @details Runs on the engine-loop thread -- the render thread on the macOS Metal path.
        * The swapchain destroys its back-buffer textures immediately rather than through the
        * deferred reclaimer, so the drain here is what makes that safe.
        */
        void recreateSwapChain(VkmWindowContext& windowContext, uint64_t packedExtent);

        /*
        * @brief Reads the given back buffer back and puts the image into the OS clipboard.
        * @details Blocks: readbackTexture submits and waits. Call it between a render graph's
        * execute() and the swapchain present, where the back buffer holds the finished frame and
        * has not been presented yet. Supported on macOS (Metal) and Windows (Vulkan); on any
        * other platform or backend it only logs.
        * @param backBuffer Back buffer acquired for the current frame.
        * @param format Format of that back buffer.
        */
        void captureBackBufferToClipboard(VkmResourceHandle backBuffer, VkmFormat format);

#if defined(VKM_ENABLE_IMGUI)
        /*
        * @brief Draws the engine-wide debug overlay (FPS, CPU usage, GPU frame time, frame
        * index), pinned top-left. Called from update() -- before the frame's first
        * ImGui::Render() call inside the ImGui renderer -- so it applies uniformly to every
        * sample with zero per-sample code and coexists with a sample's own ImGui widgets.
        */
        void renderDebugOverlay(const double deltaTime);
#endif
        
    public:
        /*
         * @brief returns the swapchain for the given window index (added via addSwapChain)
         */
        inline VkmSwapChainBase* getSwapChain(uint32_t windowIndex)
        {
            return (windowIndex < _windowContexts.size()) ? _windowContexts[windowIndex]._swapChain : nullptr;
        }

        /*
         * @brief returns engine's main (first) swapchain, or nullptr if none added yet
         */
        inline VkmSwapChainBase* getMainSwapChain() { return getSwapChain(0); }

        /*
         * @brief number of windows currently owned by the engine
         */
        inline uint32_t getWindowCount() const { return static_cast<uint32_t>(_windowContexts.size()); }

        /*
        * @brief Maps a native window handle (GLFWwindow* / CAMetalLayer*, whatever was passed
        * as VkmWindowInfo::_windowHandle) back to its window index, or kVkmNoFocusedWindow if
        * it belongs to no window of this engine.
        */
        uint32_t findWindowIndex(const void* nativeHandle) const;

        /*
        * @brief Platform layers report keyboard focus changes here, by native handle so they
        * never have to track window indices themselves. Unknown handles are ignored.
        */
        void onWindowFocusChanged(const void* nativeHandle, bool focused);

        /*
        * @brief Platform layers report a window's new back-buffer size here, by native handle so
        * they never have to track window indices themselves. Unknown handles are ignored.
        * @details Callable from the window thread: the size is only published (see
        * VkmWindowContext::_pendingExtent) and the swapchain is rebuilt later, from the engine
        * loop. Width/height are in pixels, not points -- on GLFW that is glfwGetFramebufferSize,
        * on AppKit the view's backing size. A zero extent (a minimized window) is valid and
        * simply stops that window rendering until it comes back.
        */
        void onWindowResized(const void* nativeHandle, uint32_t width, uint32_t height);

        /*
        * @brief Platform layers bracket a user-driven resize drag with this, so rendering can be
        * suspended for its duration and the swapchain rebuilt exactly once when it ends.
        * @details Only platforms that report such brackets call it (AppKit's
        * windowWillStartLiveResize/windowDidEndLiveResize). GLFW has no equivalent, and gets by
        * without one because its size callback only fires from glfwPollEvents() -- see
        * installGlfwWindowResizeCallback() for what that does and does not buy.
        */
        void onWindowLiveResizeChanged(const void* nativeHandle, bool active);

        /*
        * @brief True while the given window renders nothing -- during a live resize, or when it
        * is minimized. Platform layers use this to skip work that would otherwise stall on a
        * frame that will never be presented (see the ImGui drawable in the macOS display-link
        * callback).
        * @details Engine-loop thread only, unlike the two above: it reads the swapchain extent,
        * which the loop itself rewrites during a rebuild.
        */
        bool isWindowRenderingSuspended(uint32_t windowIndex) const;

        /*
        * @brief The window that currently holds keyboard focus, or kVkmNoFocusedWindow.
        */
        inline uint32_t getFocusedWindowIndex() const { return _inputHandler.getFocusedWindowIndex(); }

        /*
        * @brief returns engine's launch options
        */
        inline const VkmEngineLaunchOptions& getEngineOptions() const { return _engineOptions; }

        /*
        * @brief returns engine's pipeline state manager
        */
        inline VkmPipelineStateManager* getPipelineStateManager() const { return _pipelineStateManager.get(); }

        /*
        * @brief returns engine's renderer backend driver
        */
        inline VkmDriverBase* getDriver() const { return _driver; }

        /*
        * @brief returns engine's input handler for platform layers to forward key events into
        */
        inline VkmInputHandler& getInputHandler() { return _inputHandler; }

        /*
        * @brief Registers the camera whose matrices the engine publishes into descriptor set 1
        * each frame (see renderer/backend/common/frame_constants.h). Non-owning: the camera
        * must outlive the engine loop. Pass nullptr to stop publishing, which leaves set 1
        * carrying identity matrices.
        * @details The engine also drives the camera's viewport from the main swapchain's
        * extent. There is one set-1 region per frame, engine-wide, so with several windows
        * every window renders with the main swapchain's aspect ratio.
        */
        inline void setActiveCamera(VkmCamera* camera) { _activeCamera = camera; }
        inline VkmCamera* getActiveCamera() const { return _activeCamera; }

        /*
        * @brief The engine-wide anti-aliasing / upscale preset, cycled by F2.
        * @details Every mode but Off runs the temporal upscaler, Native at the display extent as
        * pure anti-aliasing. Native is the default wherever the driver reports
        * VkmDriverCapabilityFlags::TemporalUpscaling and the app consumes the mode; anywhere else
        * this is pinned to Off.
        */
        inline VkmUpscaleMode getUpscaleMode() const { return _upscaleMode; }

        /*
        * @brief Selects a preset, clamped to Off where upscaling is unavailable.
        * @details A change is not free: every consumer rebuilds its render-extent-sized targets
        * and its upscaler. Ignored when the mode already holds that value.
        * @param mode Preset to select.
        */
        void setUpscaleMode(VkmUpscaleMode mode);

        /*
        * @brief Whether any mode other than Off would do anything in this run.
        */
        inline bool isUpscaleModeAvailable() const { return _upscaleModeAvailable; }

        /*
        * @brief The extent the scene renders at: the main swapchain's, scaled by the current mode.
        * @details The camera's viewport and set 1's _viewportSize follow this, so an app that
        * sizes its scene targets from it stays consistent with the engine by construction.
        * @return Render extent in pixels, or zero while no swapchain exists or the window is
        * minimized.
        */
        glm::uvec2 getRenderExtent();

        /*
        * @brief engine loop exit condition. True once the input handler has received an exit request.
        */
        inline bool shouldExit() const { return _inputHandler.shouldExit(); }

        /*
        * @brief True while the ImGui overlay owns keyboard/mouse input, so platform layers
        * should not forward the event to the input handler.
        * @details Deliberately not inline: keeping the ImGui lookup inside engine.cpp is what
        * lets platform code query capture state without ever including an ImGui header.
        * Always false when VKM_ENABLE_IMGUI is off.
        */
        bool wantsCaptureKeyboard() const;
        bool wantsCaptureMouse() const;

        /*
        * @brief returns the engine-owned render graph capture (see render_graph_capture.h).
        * Armed via the F10 hotkey, --capture-render-graph, or arm() directly.
        */
        inline VkmRenderGraphCapture* getRenderGraphCapture() const { return _renderGraphCapture.get(); }

    public:

        /*
        * @brief Parse engine launch options from command line arguments
        */
        static VkmEngineLaunchOptions parseEngineLaunchOptions(int argc, char* argv[]);

    private:
        VkmDriverBase* _driver;
        double _lastUpdateTime;

        VkmInputHandler _inputHandler;

        VkmCamera* _activeCamera {nullptr}; // non-owning, see setActiveCamera()
        VkmUpscaleMode _upscaleMode {VkmUpscaleMode::Off};
        // Both halves of "would a mode other than Off do anything here": the driver has an
        // upscaler, and the app renders at getRenderExtent().
        bool _upscaleModeAvailable {false};

        // One entry per window; each owns its swapchain and per-frame-slot render graphs.
        // A deque, not a vector: VkmWindowContext holds atomics (so it is neither copyable nor
        // movable), and a deque never relocates the elements it already holds.
        std::deque<VkmWindowContext> _windowContexts;
        // Index into _windowContexts of the ImGui-bound window, or INVALID_VALUE32 if none.
        uint32_t _imGuiWindowIndex {INVALID_VALUE32};

        std::unique_ptr<VkmPipelineStateManager> _pipelineStateManager;

#if defined(VKM_ENABLE_IMGUI)
        std::unique_ptr<VkmImGuiRendererBase> _imGuiRenderer;
#endif

    private:
        std::unique_ptr<AppDelegate> _appDelegate;
        VkmEngineLaunchOptions _engineOptions {};

        uint32_t _currentFrameIndex {0}; // current frame slot, shared across all windows

        /*
        * @brief Frame-to-frame state the per-frame constants need but no camera can supply.
        *
        * _frameCounter is monotonic, unlike _currentFrameIndex which cycles 0..FRAME_COUNT-1:
        * stochastic passes seed from it to decorrelate successive frames.
        * _prevViewProjection carries last frame's jitter-free matrix for reprojection;
        * _hasPrevViewProjection keeps the first frame after a camera appears from reprojecting
        * against an identity matrix, which would look like a violent camera cut. _prevJitter is
        * last frame's sub-pixel jitter, published as _jitter.zw alongside it.
        */
        uint32_t _frameCounter {0};
        glm::mat4 _prevViewProjection {1.0f};
        glm::vec4 _prevCameraPositionWorld {0.0f, 0.0f, 0.0f, 1.0f};
        glm::vec2 _prevJitter {0.0f};
        bool _hasPrevViewProjection {false};

        std::unique_ptr<VkmRenderGraphCapture> _renderGraphCapture;

        // Set by the F3 hotkey, consumed by render() on the next primary-window frame.
        bool _clipboardCaptureArmed {false};

#if defined(VKM_ENABLE_IMGUI)
        double _fpsSmoothed {0.0}; // exponential moving average, used by renderDebugOverlay()
        std::unique_ptr<VkmRenderGraphInspector> _renderGraphInspector;
        std::unique_ptr<VkmMemoryInspector> _memoryInspector;
        std::unique_ptr<VkmCpuProfilerInspector> _cpuProfilerInspector;
        std::unique_ptr<VkmGpuProfilerInspector> _gpuProfilerInspector;
        std::unique_ptr<VkmAccelerationStructureInspector> _accelerationStructureInspector;
        // The inspector's 3D view. Null on a backend without ray tracing, where no structure
        // can exist for it to outline.
        std::unique_ptr<VkmAccelerationStructureDebugRenderer> _asDebugRenderer;
        // Previous frame's profiler window visibility, so update() can start/stop capture on
        // the edge instead of overriding the inspector's own Start/Stop button every frame.
        bool _cpuProfilerWasVisible {false};
        bool _gpuProfilerWasVisible {false};
#endif
    };
}
