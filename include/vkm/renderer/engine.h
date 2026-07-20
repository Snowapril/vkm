// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/platform/common/app_delegate.h>
#include <vkm/platform/common/input_handler.h>
#include <vkm/platform/common/window.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <memory>

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

    struct VkmEngineLaunchOptions
    {
        bool enableValidationLayer;
        bool enableGpuCapture = false;
        bool enableGpuCrashDump = false;
        // Arm a render graph capture at startup so the first rendered frame is captured
        // (equivalent to pressing the capture hotkey before frame 0).
        bool captureRenderGraphOnStartup = false;
        // Capture the first rendered frame to a .gputrace file at startup (Metal only;
        // implies enableGpuCapture). Equivalent to pressing F9 before frame 0.
        bool captureGpuFrameOnStartup = false;
    };
    constexpr const VkmEngineLaunchOptions DEFAULT_ENGINE_LAUNCH_OPTIONS = { true, false, false, false, false };

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
        * @brief Add swapchain to engine
        */
        void addSwapChain(const VkmWindowInfo& windowInfo);
        
    private:
        /*
         
         */
        void update(const double deltaTime);

        /*

         */
        void render(const double deltaTime);
        void prepareRender();

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
         * @brief returns engine's main swapchain created
         */
        inline VkmSwapChainBase* getMainSwapChain() { return _mainSwapChain; }

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

        VkmSwapChainBase* _mainSwapChain {nullptr}; // main swapchain. engine should have multiple swapchains but at now, only one swapchain is supported.

        std::unique_ptr<VkmPipelineStateManager> _pipelineStateManager;

#if defined(VKM_ENABLE_IMGUI)
        std::unique_ptr<VkmImGuiRendererBase> _imGuiRenderer;
#endif

    private:
        std::unique_ptr<AppDelegate> _appDelegate;
        VkmEngineLaunchOptions _engineOptions {};

        std::array<std::unique_ptr<VkmRenderGraph>, FRAME_COUNT> _frameRenderGraphs;
        uint32_t _currentFrameIndex {0}; // current frame index for render graph

        std::unique_ptr<VkmRenderGraphCapture> _renderGraphCapture;

#if defined(VKM_ENABLE_IMGUI)
        double _fpsSmoothed {0.0}; // exponential moving average, used by renderDebugOverlay()
        std::unique_ptr<VkmRenderGraphInspector> _renderGraphInspector;
#endif
    };
}
