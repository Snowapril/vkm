// Copyright (c) 2025 Snowapril

#include <vkm/platform/common/input_handler.h>

namespace vkm
{
    void VkmInputHandler::onKeyEvent(VkmKeyCode key, VkmKeyAction action)
    {
        if (key == VkmKeyCode::Escape && action == VkmKeyAction::Press)
        {
            _exitRequested = true;
        }
    }
}
