// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/platform/common/app_delegate.h>
#include <vkm/platform/common/input_handler.h>
#include <vkm/platform/common/window.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <memory>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmTexture;
    class VkmSwapChainBase;
    class VkmPipelineStateManager;
    class VkmRenderGraphCapture;
#if defined(VKM_ENABLE_IMGUI)
    class VkmImGuiRendererBase;
    class VkmRenderGraphInspector;
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
    };
    constexpr const VkmEngineLaunchOptions DEFAULT_ENGINE_LAUNCH_OPTIONS = { true, false, false, false, false, 0, 1, false };

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

        // One entry per window; each owns its swapchain and per-frame-slot render graphs.
        std::vector<VkmWindowContext> _windowContexts;
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

        std::unique_ptr<VkmRenderGraphCapture> _renderGraphCapture;

#if defined(VKM_ENABLE_IMGUI)
        double _fpsSmoothed {0.0}; // exponential moving average, used by renderDebugOverlay()
        std::unique_ptr<VkmRenderGraphInspector> _renderGraphInspector;
#endif
    };
}
