// Copyright (c) 2025 Snowapril

#include <vkm/platform/common/input_handler.h>

#include <vkm/base/common.h>

#include <algorithm>
#include <iterator>

namespace vkm
{
    const char* toString(VkmKeyCode key)
    {
        static const char* const KEY_NAMES[] =
        {
            #define VKM_KEY_CODE_NAME(name) #name,
            VKM_KEY_CODE_LIST(VKM_KEY_CODE_NAME)
            #undef VKM_KEY_CODE_NAME
        };

        const size_t index = static_cast<size_t>(key);
        if (index >= std::size(KEY_NAMES))
        {
            return "Unknown";
        }

        return KEY_NAMES[index];
    }

    const char* toString(VkmMouseButton button)
    {
        switch (button)
        {
            case VkmMouseButton::Left:    return "Left";
            case VkmMouseButton::Right:   return "Right";
            case VkmMouseButton::Middle:  return "Middle";
            case VkmMouseButton::Button4: return "Button4";
            case VkmMouseButton::Button5: return "Button5";
            default:                      return "Unknown";
        }
    }

    const char* toString(VkmKeyAction action)
    {
        switch (action)
        {
            case VkmKeyAction::Press:   return "Press";
            case VkmKeyAction::Release: return "Release";
            default:                    return "Unknown";
        }
    }

    void VkmInputHandler::onKeyEvent(VkmKeyCode key, VkmKeyAction action, uint32_t modifiers, bool isRepeat)
    {
        // Exit is latched here rather than in beginFrame() so that the GLFW while loop and the
        // Metal display-link shouldExit() check both react on the same frame as the key press,
        // regardless of when the event queue happens to be drained.
        if (key == VkmKeyCode::Escape && action == VkmKeyAction::Press)
        {
            _exitRequested.store(true, std::memory_order_relaxed);
        }

        VkmInputEvent event;
        event._type = VkmInputEventType::Key;
        event._key = key;
        event._action = action;
        event._modifiers = modifiers;
        event._isRepeat = isRepeat;
        pushEvent(event);
    }

    void VkmInputHandler::onMouseButtonEvent(VkmMouseButton button, VkmKeyAction action, uint32_t modifiers)
    {
        VkmInputEvent event;
        event._type = VkmInputEventType::MouseButton;
        event._button = button;
        event._action = action;
        event._modifiers = modifiers;
        pushEvent(event);
    }

    void VkmInputHandler::onCursorMove(double x, double y)
    {
        VkmInputEvent event;
        event._type = VkmInputEventType::CursorMove;
        event._x = x;
        event._y = y;
        pushEvent(event);
    }

    void VkmInputHandler::onScroll(double offsetX, double offsetY)
    {
        VkmInputEvent event;
        event._type = VkmInputEventType::Scroll;
        event._x = offsetX;
        event._y = offsetY;
        pushEvent(event);
    }

    void VkmInputHandler::pushEvent(const VkmInputEvent& event)
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        _pendingEvents.push_back(event);
    }

    void VkmInputHandler::beginFrame()
    {
        _keyPressed.reset();
        _keyReleased.reset();
        _buttonPressed.reset();
        _buttonReleased.reset();

        _cursorDeltaX = 0.0;
        _cursorDeltaY = 0.0;
        _scrollDeltaX = 0.0;
        _scrollDeltaY = 0.0;

        // Drained into a local so listeners may push further events or (un)register listeners
        // without deadlocking on _pendingMutex.
        std::vector<VkmInputEvent> events;
        {
            std::lock_guard<std::mutex> lock(_pendingMutex);
            events.swap(_pendingEvents);
        }

        for (const VkmInputEvent& event : events)
        {
            applyEvent(event);
            logEvent(event);

            for (const std::pair<uint32_t, VkmInputListener>& listener : _listeners)
            {
                listener.second(event);
            }
        }
    }

    void VkmInputHandler::applyEvent(const VkmInputEvent& event)
    {
        switch (event._type)
        {
            case VkmInputEventType::Key:
            {
                _modifiers = event._modifiers;

                const size_t index = static_cast<size_t>(event._key);
                if (index >= static_cast<size_t>(VkmKeyCode::Count))
                {
                    break;
                }

                if (event._action == VkmKeyAction::Press)
                {
                    // Auto-repeats keep the key down but must not re-trigger the pressed edge.
                    if (event._isRepeat == false)
                    {
                        _keyPressed.set(index);
                    }
                    _keyDown.set(index);
                }
                else
                {
                    _keyReleased.set(index);
                    _keyDown.reset(index);
                }
                break;
            }
            case VkmInputEventType::MouseButton:
            {
                _modifiers = event._modifiers;

                const size_t index = static_cast<size_t>(event._button);
                if (index >= static_cast<size_t>(VkmMouseButton::Count))
                {
                    break;
                }

                if (event._action == VkmKeyAction::Press)
                {
                    _buttonPressed.set(index);
                    _buttonDown.set(index);
                }
                else
                {
                    _buttonReleased.set(index);
                    _buttonDown.reset(index);
                }
                break;
            }
            case VkmInputEventType::CursorMove:
            {
                if (_cursorInitialized)
                {
                    _cursorDeltaX += event._x - _cursorX;
                    _cursorDeltaY += event._y - _cursorY;
                }

                _cursorX = event._x;
                _cursorY = event._y;
                _cursorInitialized = true;
                break;
            }
            case VkmInputEventType::Scroll:
            {
                _scrollDeltaX += event._x;
                _scrollDeltaY += event._y;
                break;
            }
        }
    }

    void VkmInputHandler::logEvent(const VkmInputEvent& event) const
    {
        switch (event._type)
        {
            case VkmInputEventType::Key:
            {
                VKM_DEBUG_LOG(fmt::format("[Input] Key {} {}{} mods=0x{:X}",
                    toString(event._key), toString(event._action),
                    event._isRepeat ? " (repeat)" : "", event._modifiers).c_str());
                break;
            }
            case VkmInputEventType::MouseButton:
            {
                VKM_DEBUG_LOG(fmt::format("[Input] MouseButton {} {} mods=0x{:X}",
                    toString(event._button), toString(event._action), event._modifiers).c_str());
                break;
            }
            case VkmInputEventType::CursorMove:
            {
                if (_verboseCursorLogging)
                {
                    VKM_DEBUG_LOG(fmt::format("[Input] CursorMove x={:.1f} y={:.1f}",
                        event._x, event._y).c_str());
                }
                break;
            }
            case VkmInputEventType::Scroll:
            {
                VKM_DEBUG_LOG(fmt::format("[Input] Scroll dx={:.3f} dy={:.3f}",
                    event._x, event._y).c_str());
                break;
            }
        }
    }

    uint32_t VkmInputHandler::addListener(VkmInputListener listener)
    {
        const uint32_t handle = _nextListenerHandle++;
        _listeners.emplace_back(handle, std::move(listener));
        return handle;
    }

    void VkmInputHandler::removeListener(uint32_t listenerHandle)
    {
        _listeners.erase(std::remove_if(_listeners.begin(), _listeners.end(),
            [listenerHandle](const std::pair<uint32_t, VkmInputListener>& entry)
            {
                return entry.first == listenerHandle;
            }), _listeners.end());
    }
}
