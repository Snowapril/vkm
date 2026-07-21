// Copyright (c) 2025 Snowapril

#include <vkm/platform/common/glfw_input.h>

// The macOS Metal path drives AppKit directly and does not link GLFW, so the whole
// translation unit collapses to an empty stub there. Keeping the guard here rather than in
// CMake lets the file sit in the unconditional source list.
#if !defined(VKM_USE_METAL_API)

#include <vkm/platform/common/input_handler.h>
#include <vkm/renderer/engine.h>

#include <GLFW/glfw3.h>

namespace vkm
{
    namespace
    {
        /*
        * @brief Previously installed GLFW handlers, which belong to the ImGui backend.
        * @details File-static because the engine is single-window by construction
        * (VkmEngine::addSwapChain asserts that no main swapchain exists yet).
        */
        struct GlfwPreviousCallbacks
        {
            GLFWkeyfun _key {nullptr};
            GLFWcharfun _char {nullptr};
            GLFWmousebuttonfun _mouseButton {nullptr};
            GLFWcursorposfun _cursorPos {nullptr};
            GLFWscrollfun _scroll {nullptr};
        };

        GlfwPreviousCallbacks g_previousCallbacks;

        VkmEngine* getEngine(GLFWwindow* window)
        {
            return static_cast<VkmEngine*>(glfwGetWindowUserPointer(window));
        }

        VkmKeyCode glfwKeyToVkmKeyCode(int key)
        {
            switch (key)
            {
                case GLFW_KEY_A: return VkmKeyCode::A;
                case GLFW_KEY_B: return VkmKeyCode::B;
                case GLFW_KEY_C: return VkmKeyCode::C;
                case GLFW_KEY_D: return VkmKeyCode::D;
                case GLFW_KEY_E: return VkmKeyCode::E;
                case GLFW_KEY_F: return VkmKeyCode::F;
                case GLFW_KEY_G: return VkmKeyCode::G;
                case GLFW_KEY_H: return VkmKeyCode::H;
                case GLFW_KEY_I: return VkmKeyCode::I;
                case GLFW_KEY_J: return VkmKeyCode::J;
                case GLFW_KEY_K: return VkmKeyCode::K;
                case GLFW_KEY_L: return VkmKeyCode::L;
                case GLFW_KEY_M: return VkmKeyCode::M;
                case GLFW_KEY_N: return VkmKeyCode::N;
                case GLFW_KEY_O: return VkmKeyCode::O;
                case GLFW_KEY_P: return VkmKeyCode::P;
                case GLFW_KEY_Q: return VkmKeyCode::Q;
                case GLFW_KEY_R: return VkmKeyCode::R;
                case GLFW_KEY_S: return VkmKeyCode::S;
                case GLFW_KEY_T: return VkmKeyCode::T;
                case GLFW_KEY_U: return VkmKeyCode::U;
                case GLFW_KEY_V: return VkmKeyCode::V;
                case GLFW_KEY_W: return VkmKeyCode::W;
                case GLFW_KEY_X: return VkmKeyCode::X;
                case GLFW_KEY_Y: return VkmKeyCode::Y;
                case GLFW_KEY_Z: return VkmKeyCode::Z;

                case GLFW_KEY_0: return VkmKeyCode::Num0;
                case GLFW_KEY_1: return VkmKeyCode::Num1;
                case GLFW_KEY_2: return VkmKeyCode::Num2;
                case GLFW_KEY_3: return VkmKeyCode::Num3;
                case GLFW_KEY_4: return VkmKeyCode::Num4;
                case GLFW_KEY_5: return VkmKeyCode::Num5;
                case GLFW_KEY_6: return VkmKeyCode::Num6;
                case GLFW_KEY_7: return VkmKeyCode::Num7;
                case GLFW_KEY_8: return VkmKeyCode::Num8;
                case GLFW_KEY_9: return VkmKeyCode::Num9;

                case GLFW_KEY_F1:  return VkmKeyCode::F1;
                case GLFW_KEY_F2:  return VkmKeyCode::F2;
                case GLFW_KEY_F3:  return VkmKeyCode::F3;
                case GLFW_KEY_F4:  return VkmKeyCode::F4;
                case GLFW_KEY_F5:  return VkmKeyCode::F5;
                case GLFW_KEY_F6:  return VkmKeyCode::F6;
                case GLFW_KEY_F7:  return VkmKeyCode::F7;
                case GLFW_KEY_F8:  return VkmKeyCode::F8;
                case GLFW_KEY_F9:  return VkmKeyCode::F9;
                case GLFW_KEY_F10: return VkmKeyCode::F10;
                case GLFW_KEY_F11: return VkmKeyCode::F11;
                case GLFW_KEY_F12: return VkmKeyCode::F12;

                case GLFW_KEY_LEFT:  return VkmKeyCode::Left;
                case GLFW_KEY_RIGHT: return VkmKeyCode::Right;
                case GLFW_KEY_UP:    return VkmKeyCode::Up;
                case GLFW_KEY_DOWN:  return VkmKeyCode::Down;

                case GLFW_KEY_LEFT_SHIFT:     return VkmKeyCode::LeftShift;
                case GLFW_KEY_RIGHT_SHIFT:    return VkmKeyCode::RightShift;
                case GLFW_KEY_LEFT_CONTROL:   return VkmKeyCode::LeftControl;
                case GLFW_KEY_RIGHT_CONTROL:  return VkmKeyCode::RightControl;
                case GLFW_KEY_LEFT_ALT:       return VkmKeyCode::LeftAlt;
                case GLFW_KEY_RIGHT_ALT:      return VkmKeyCode::RightAlt;
                case GLFW_KEY_LEFT_SUPER:     return VkmKeyCode::LeftSuper;
                case GLFW_KEY_RIGHT_SUPER:    return VkmKeyCode::RightSuper;

                case GLFW_KEY_SPACE:     return VkmKeyCode::Space;
                case GLFW_KEY_ENTER:     return VkmKeyCode::Enter;
                case GLFW_KEY_TAB:       return VkmKeyCode::Tab;
                case GLFW_KEY_BACKSPACE: return VkmKeyCode::Backspace;
                case GLFW_KEY_DELETE:    return VkmKeyCode::Delete;
                case GLFW_KEY_ESCAPE:    return VkmKeyCode::Escape;

                case GLFW_KEY_INSERT:    return VkmKeyCode::Insert;
                case GLFW_KEY_HOME:      return VkmKeyCode::Home;
                case GLFW_KEY_END:       return VkmKeyCode::End;
                case GLFW_KEY_PAGE_UP:   return VkmKeyCode::PageUp;
                case GLFW_KEY_PAGE_DOWN: return VkmKeyCode::PageDown;
                case GLFW_KEY_CAPS_LOCK: return VkmKeyCode::CapsLock;

                case GLFW_KEY_MINUS:         return VkmKeyCode::Minus;
                case GLFW_KEY_EQUAL:         return VkmKeyCode::Equal;
                case GLFW_KEY_LEFT_BRACKET:  return VkmKeyCode::LeftBracket;
                case GLFW_KEY_RIGHT_BRACKET: return VkmKeyCode::RightBracket;
                case GLFW_KEY_BACKSLASH:     return VkmKeyCode::Backslash;
                case GLFW_KEY_SEMICOLON:     return VkmKeyCode::Semicolon;
                case GLFW_KEY_APOSTROPHE:    return VkmKeyCode::Apostrophe;
                case GLFW_KEY_GRAVE_ACCENT:  return VkmKeyCode::GraveAccent;
                case GLFW_KEY_COMMA:         return VkmKeyCode::Comma;
                case GLFW_KEY_PERIOD:        return VkmKeyCode::Period;
                case GLFW_KEY_SLASH:         return VkmKeyCode::Slash;

                case GLFW_KEY_KP_0: return VkmKeyCode::Keypad0;
                case GLFW_KEY_KP_1: return VkmKeyCode::Keypad1;
                case GLFW_KEY_KP_2: return VkmKeyCode::Keypad2;
                case GLFW_KEY_KP_3: return VkmKeyCode::Keypad3;
                case GLFW_KEY_KP_4: return VkmKeyCode::Keypad4;
                case GLFW_KEY_KP_5: return VkmKeyCode::Keypad5;
                case GLFW_KEY_KP_6: return VkmKeyCode::Keypad6;
                case GLFW_KEY_KP_7: return VkmKeyCode::Keypad7;
                case GLFW_KEY_KP_8: return VkmKeyCode::Keypad8;
                case GLFW_KEY_KP_9: return VkmKeyCode::Keypad9;

                case GLFW_KEY_KP_DECIMAL:  return VkmKeyCode::KeypadDecimal;
                case GLFW_KEY_KP_DIVIDE:   return VkmKeyCode::KeypadDivide;
                case GLFW_KEY_KP_MULTIPLY: return VkmKeyCode::KeypadMultiply;
                case GLFW_KEY_KP_SUBTRACT: return VkmKeyCode::KeypadSubtract;
                case GLFW_KEY_KP_ADD:      return VkmKeyCode::KeypadAdd;
                case GLFW_KEY_KP_ENTER:    return VkmKeyCode::KeypadEnter;
                case GLFW_KEY_KP_EQUAL:    return VkmKeyCode::KeypadEqual;

                default: return VkmKeyCode::Unknown;
            }
        }

        uint32_t glfwModsToVkmModifiers(int mods)
        {
            uint32_t modifiers = 0;
            if ((mods & GLFW_MOD_SHIFT) != 0)   { modifiers |= static_cast<uint32_t>(VkmInputModifier::Shift); }
            if ((mods & GLFW_MOD_CONTROL) != 0) { modifiers |= static_cast<uint32_t>(VkmInputModifier::Control); }
            if ((mods & GLFW_MOD_ALT) != 0)     { modifiers |= static_cast<uint32_t>(VkmInputModifier::Alt); }
            if ((mods & GLFW_MOD_SUPER) != 0)   { modifiers |= static_cast<uint32_t>(VkmInputModifier::Super); }
            return modifiers;
        }

        VkmMouseButton glfwButtonToVkmMouseButton(int button)
        {
            switch (button)
            {
                case GLFW_MOUSE_BUTTON_LEFT:   return VkmMouseButton::Left;
                case GLFW_MOUSE_BUTTON_RIGHT:  return VkmMouseButton::Right;
                case GLFW_MOUSE_BUTTON_MIDDLE: return VkmMouseButton::Middle;
                case GLFW_MOUSE_BUTTON_4:      return VkmMouseButton::Button4;
                case GLFW_MOUSE_BUTTON_5:      return VkmMouseButton::Button5;
                default:                       return VkmMouseButton::Count;
            }
        }

        void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
        {
            if (g_previousCallbacks._key != nullptr)
            {
                g_previousCallbacks._key(window, key, scancode, action, mods);
            }

            VkmEngine* engine = getEngine(window);
            if (engine == nullptr || engine->wantsCaptureKeyboard())
            {
                return;
            }

            const VkmKeyCode keyCode = glfwKeyToVkmKeyCode(key);
            if (keyCode == VkmKeyCode::Unknown)
            {
                return;
            }

            if (action == GLFW_PRESS || action == GLFW_REPEAT)
            {
                engine->getInputHandler().onKeyEvent(keyCode, VkmKeyAction::Press,
                    glfwModsToVkmModifiers(mods), action == GLFW_REPEAT);
            }
            else if (action == GLFW_RELEASE)
            {
                engine->getInputHandler().onKeyEvent(keyCode, VkmKeyAction::Release,
                    glfwModsToVkmModifiers(mods));
            }
        }

        void charCallback(GLFWwindow* window, unsigned int codepoint)
        {
            // Text input is ImGui's alone today; the engine has no text consumer. Forwarding
            // here exists purely so installing our key callback does not detach ImGui's.
            if (g_previousCallbacks._char != nullptr)
            {
                g_previousCallbacks._char(window, codepoint);
            }
        }

        void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
        {
            if (g_previousCallbacks._mouseButton != nullptr)
            {
                g_previousCallbacks._mouseButton(window, button, action, mods);
            }

            VkmEngine* engine = getEngine(window);
            if (engine == nullptr || engine->wantsCaptureMouse())
            {
                return;
            }

            const VkmMouseButton mouseButton = glfwButtonToVkmMouseButton(button);
            if (mouseButton == VkmMouseButton::Count)
            {
                return;
            }

            engine->getInputHandler().onMouseButtonEvent(mouseButton,
                action == GLFW_PRESS ? VkmKeyAction::Press : VkmKeyAction::Release,
                glfwModsToVkmModifiers(mods));
        }

        void cursorPosCallback(GLFWwindow* window, double x, double y)
        {
            if (g_previousCallbacks._cursorPos != nullptr)
            {
                g_previousCallbacks._cursorPos(window, x, y);
            }

            VkmEngine* engine = getEngine(window);
            if (engine == nullptr || engine->wantsCaptureMouse())
            {
                return;
            }

            engine->getInputHandler().onCursorMove(x, y);
        }

        void scrollCallback(GLFWwindow* window, double offsetX, double offsetY)
        {
            if (g_previousCallbacks._scroll != nullptr)
            {
                g_previousCallbacks._scroll(window, offsetX, offsetY);
            }

            VkmEngine* engine = getEngine(window);
            if (engine == nullptr || engine->wantsCaptureMouse())
            {
                return;
            }

            engine->getInputHandler().onScroll(offsetX, offsetY);
        }
    }

    void installGlfwInputCallbacks(void* glfwWindow, VkmEngine* engine)
    {
        GLFWwindow* window = static_cast<GLFWwindow*>(glfwWindow);
        glfwSetWindowUserPointer(window, engine);

        // Each setter returns the handler it displaced -- ImGui's, since addSwapChain()
        // initializes the ImGui GLFW backend with install_callbacks=true before we get here.
        // Our handlers forward to these first so ImGui keeps seeing every event, and only then
        // consult wantsCaptureKeyboard/Mouse to decide whether the engine also gets it.
        //
        // That capture state is one frame stale: GLFW callbacks fire from glfwPollEvents()
        // before loopInner() runs ImGui's NewFrame. The upstream ImGui backends have the same
        // behaviour, so it is not worth working around.
        g_previousCallbacks._key = glfwSetKeyCallback(window, keyCallback);
        g_previousCallbacks._char = glfwSetCharCallback(window, charCallback);
        g_previousCallbacks._mouseButton = glfwSetMouseButtonCallback(window, mouseButtonCallback);
        g_previousCallbacks._cursorPos = glfwSetCursorPosCallback(window, cursorPosCallback);
        g_previousCallbacks._scroll = glfwSetScrollCallback(window, scrollCallback);
    }
}

#else

namespace vkm
{
    void installGlfwInputCallbacks(void* /*glfwWindow*/, VkmEngine* /*engine*/)
    {
        // No GLFW window on the macOS Metal path; input arrives through AppKit instead.
    }
}

#endif
