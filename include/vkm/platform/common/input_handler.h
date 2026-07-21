// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/platform/common/input_codes.h>

#include <atomic>
#include <bitset>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace vkm
{
    enum class VkmInputEventType
    {
        Key,
        MouseButton,
        CursorMove,
        Scroll,
    };

    /*
    * @brief One discrete input event as delivered to listeners.
    * @details Only the fields relevant to _type carry meaning; the rest keep their defaults.
    */
    struct VkmInputEvent
    {
        VkmInputEventType _type {VkmInputEventType::Key};
        VkmKeyCode _key {VkmKeyCode::Unknown};
        VkmMouseButton _button {VkmMouseButton::Left};
        VkmKeyAction _action {VkmKeyAction::Press};
        uint32_t _modifiers {0};
        double _x {0.0}; // CursorMove: absolute position. Scroll: offset.
        double _y {0.0};
        bool _isRepeat {false};
    };

    using VkmInputListener = std::function<void(const VkmInputEvent&)>;

    /*
    * @brief Platform-agnostic input handler owned by the engine.
    * @details Platform layers translate their native events into the onXxx() calls below,
    * which may run on a different thread than the engine loop: the macOS Metal path receives
    * AppKit events on the main thread while loopInner() runs on the CAMetalDisplayLink render
    * thread. Pushed events are therefore buffered under a mutex and folded into the queryable
    * state exactly once per frame, from beginFrame().
    */
    class VkmInputHandler
    {
    public:
        // --- producer side: called from platform callbacks, any thread ---------------

        void onKeyEvent(VkmKeyCode key, VkmKeyAction action, uint32_t modifiers = 0, bool isRepeat = false);
        void onMouseButtonEvent(VkmMouseButton button, VkmKeyAction action, uint32_t modifiers = 0);
        void onCursorMove(double x, double y);
        void onScroll(double offsetX, double offsetY);

        // --- consumer side: called from the engine loop thread only ------------------

        /*
        * @brief Clears the previous frame's edge and delta state, then drains and applies
        * every event buffered since the last call, logging and dispatching listeners as it goes.
        */
        void beginFrame();

        inline bool isKeyDown(VkmKeyCode key) const { return _keyDown.test(static_cast<size_t>(key)); }
        inline bool isKeyPressed(VkmKeyCode key) const { return _keyPressed.test(static_cast<size_t>(key)); }
        inline bool isKeyReleased(VkmKeyCode key) const { return _keyReleased.test(static_cast<size_t>(key)); }

        inline bool isMouseButtonDown(VkmMouseButton button) const { return _buttonDown.test(static_cast<size_t>(button)); }
        inline bool isMouseButtonPressed(VkmMouseButton button) const { return _buttonPressed.test(static_cast<size_t>(button)); }
        inline bool isMouseButtonReleased(VkmMouseButton button) const { return _buttonReleased.test(static_cast<size_t>(button)); }

        inline uint32_t getModifiers() const { return _modifiers; }
        inline bool hasModifier(VkmInputModifier modifier) const { return (_modifiers & static_cast<uint32_t>(modifier)) != 0; }

        inline double getCursorX() const { return _cursorX; }
        inline double getCursorY() const { return _cursorY; }
        inline double getCursorDeltaX() const { return _cursorDeltaX; }
        inline double getCursorDeltaY() const { return _cursorDeltaY; }
        inline double getScrollDeltaX() const { return _scrollDeltaX; }
        inline double getScrollDeltaY() const { return _scrollDeltaY; }

        /*
        * @brief Subscribes to every input event. Listeners are invoked from beginFrame(),
        * so always on the engine loop thread and never from a platform callback thread.
        * @details Returns a handle to pass to removeListener().
        */
        uint32_t addListener(VkmInputListener listener);
        void removeListener(uint32_t listenerHandle);

        inline bool shouldExit() const { return _exitRequested.load(std::memory_order_relaxed); }

        /*
        * @brief Cursor-move events arrive at pointer report rate and would flood the debug
        * log, so they are the one event type whose logging is opt-in. Off by default.
        */
        inline void setVerboseCursorLogging(bool enabled) { _verboseCursorLogging = enabled; }

    private:
        void pushEvent(const VkmInputEvent& event);
        void applyEvent(const VkmInputEvent& event);
        void logEvent(const VkmInputEvent& event) const;

    private:
        std::mutex _pendingMutex;
        std::vector<VkmInputEvent> _pendingEvents;

        std::bitset<static_cast<size_t>(VkmKeyCode::Count)> _keyDown;
        std::bitset<static_cast<size_t>(VkmKeyCode::Count)> _keyPressed;
        std::bitset<static_cast<size_t>(VkmKeyCode::Count)> _keyReleased;

        std::bitset<static_cast<size_t>(VkmMouseButton::Count)> _buttonDown;
        std::bitset<static_cast<size_t>(VkmMouseButton::Count)> _buttonPressed;
        std::bitset<static_cast<size_t>(VkmMouseButton::Count)> _buttonReleased;

        uint32_t _modifiers {0};

        double _cursorX {0.0};
        double _cursorY {0.0};
        double _cursorDeltaX {0.0};
        double _cursorDeltaY {0.0};
        double _scrollDeltaX {0.0};
        double _scrollDeltaY {0.0};

        std::vector<std::pair<uint32_t, VkmInputListener>> _listeners;
        uint32_t _nextListenerHandle {0};

        bool _cursorInitialized {false}; // suppresses a bogus delta on the very first move
        bool _verboseCursorLogging {false};

        std::atomic<bool> _exitRequested {false};
    };
}
