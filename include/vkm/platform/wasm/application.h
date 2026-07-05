// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/engine.h>
#include <memory>

struct GLFWwindow;

namespace vkm
{
    class VkmEngine;
    class AppDelegate;

    /*
    * @brief Window class
    */
    class VkmWindow
    {
    public:
        VkmWindow();
        ~VkmWindow();

        /*
        * @brief Create window
        */
        bool create(uint32_t width, uint32_t height, const char* title);

        /*
        * @brief Destroy window
        */
        void destroy();

        /*
        * @brief Update window
        */
        void update();

        /*
        * @brief Check window receive close signal
        */
        bool shouldClose() const;

        inline GLFWwindow* getHandle() const { return _windowHandle; }

    private:
        GLFWwindow* _windowHandle;
    };

    /*
    * @brief Application class
    */
    class VkmApplication
    {
    public:
        VkmApplication();
        ~VkmApplication();

        /*
        * @brief Entry point of application
        */
        int entryPoint(AppDelegate* appDelegate, int argc, char* argv[]);

        /*
        * @brief Destroy application
        */
        void destroy();

    private:
        // Emscripten drives the loop via a callback (see wasm/AGENTS.md); a blocking
        // while() loop like the other platforms use would freeze the browser tab.
        static void mainLoopTrampoline(void* userData);

    private:
        VkmEngine _engine;
        VkmWindow _window;

        AppDelegate* _appDelegate{nullptr};
    };
}
