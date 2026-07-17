// Copyright (c) 2025 Snowapril

#pragma once

namespace vkm
{
    enum class VkmKeyCode
    {
        Escape,
    };

    enum class VkmKeyAction
    {
        Press,
        Release,
    };

    /*
    * @brief Platform-agnostic input handler owned by the engine.
    * @details Platform layers translate their native key events into VkmKeyCode/VkmKeyAction
    * and forward them here; the engine loop consults shouldExit() to decide when to stop.
    */
    class VkmInputHandler
    {
    public:
        void onKeyEvent(VkmKeyCode key, VkmKeyAction action);

        inline bool shouldExit() const { return _exitRequested; }

    private:
        bool _exitRequested {false};
    };
}
