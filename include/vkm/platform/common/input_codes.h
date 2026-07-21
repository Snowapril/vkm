// Copyright (c) 2025 Snowapril

#pragma once

#include <cstdint>

namespace vkm
{
    /*
    * @brief Single source of truth for the engine's key set.
    * @details Expanded to generate both the VkmKeyCode enumerators and their string names,
    * so adding a key never requires touching a second table. Platform -> VkmKeyCode
    * translation stays in per-platform switch statements (glfw_input.cpp and
    * platform/apple/application.mm) because GLFW_KEY_* / kVK_* constants are not visible
    * from this public header, which engine.h pulls into every translation unit.
    */
    // The parameter is named ENTRY rather than the conventional X because the list itself
    // contains an entry for the letter X: with an X parameter, X(X) would substitute the macro
    // into its own argument and never expand to the enumerator.
    #define VKM_KEY_CODE_LIST(ENTRY) \
        ENTRY(Unknown) \
        ENTRY(A) ENTRY(B) ENTRY(C) ENTRY(D) ENTRY(E) ENTRY(F) ENTRY(G) \
        ENTRY(H) ENTRY(I) ENTRY(J) ENTRY(K) ENTRY(L) ENTRY(M) ENTRY(N) \
        ENTRY(O) ENTRY(P) ENTRY(Q) ENTRY(R) ENTRY(S) ENTRY(T) ENTRY(U) \
        ENTRY(V) ENTRY(W) ENTRY(X) ENTRY(Y) ENTRY(Z) \
        ENTRY(Num0) ENTRY(Num1) ENTRY(Num2) ENTRY(Num3) ENTRY(Num4) \
        ENTRY(Num5) ENTRY(Num6) ENTRY(Num7) ENTRY(Num8) ENTRY(Num9) \
        ENTRY(F1) ENTRY(F2) ENTRY(F3) ENTRY(F4) ENTRY(F5) ENTRY(F6) \
        ENTRY(F7) ENTRY(F8) ENTRY(F9) ENTRY(F10) ENTRY(F11) ENTRY(F12) \
        ENTRY(Left) ENTRY(Right) ENTRY(Up) ENTRY(Down) \
        ENTRY(LeftShift) ENTRY(RightShift) ENTRY(LeftControl) ENTRY(RightControl) \
        ENTRY(LeftAlt) ENTRY(RightAlt) ENTRY(LeftSuper) ENTRY(RightSuper) \
        ENTRY(Space) ENTRY(Enter) ENTRY(Tab) ENTRY(Backspace) ENTRY(Delete) ENTRY(Escape) \
        ENTRY(Insert) ENTRY(Home) ENTRY(End) ENTRY(PageUp) ENTRY(PageDown) ENTRY(CapsLock) \
        ENTRY(Minus) ENTRY(Equal) ENTRY(LeftBracket) ENTRY(RightBracket) ENTRY(Backslash) \
        ENTRY(Semicolon) ENTRY(Apostrophe) ENTRY(GraveAccent) \
        ENTRY(Comma) ENTRY(Period) ENTRY(Slash) \
        ENTRY(Keypad0) ENTRY(Keypad1) ENTRY(Keypad2) ENTRY(Keypad3) ENTRY(Keypad4) \
        ENTRY(Keypad5) ENTRY(Keypad6) ENTRY(Keypad7) ENTRY(Keypad8) ENTRY(Keypad9) \
        ENTRY(KeypadDecimal) ENTRY(KeypadDivide) ENTRY(KeypadMultiply) \
        ENTRY(KeypadSubtract) ENTRY(KeypadAdd) ENTRY(KeypadEnter) ENTRY(KeypadEqual)

    enum class VkmKeyCode : uint16_t
    {
        #define VKM_KEY_CODE_ENUMERATOR(name) name,
        VKM_KEY_CODE_LIST(VKM_KEY_CODE_ENUMERATOR)
        #undef VKM_KEY_CODE_ENUMERATOR
        Count,
    };

    enum class VkmMouseButton : uint8_t
    {
        Left,
        Right,
        Middle,
        Button4,
        Button5,
        Count,
    };

    /*
    * @brief Key/button transition. Auto-repeat reuses Press together with
    * VkmInputEvent::_isRepeat instead of adding a third enumerator.
    */
    enum class VkmKeyAction
    {
        Press,
        Release,
    };

    enum class VkmInputModifier : uint32_t
    {
        None    = 0,
        Shift   = 1u << 0,
        Control = 1u << 1,
        Alt     = 1u << 2,
        Super   = 1u << 3,
    };

    /*
    * @brief Human-readable enumerator names, used by the input event debug logs.
    */
    const char* toString(VkmKeyCode key);
    const char* toString(VkmMouseButton button);
    const char* toString(VkmKeyAction action);
}
