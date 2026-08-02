# Apple Platform Layer

Targets: **macOS Metal4** (`IOS=OFF`) and **iOS Metal4** (`IOS=ON`). Both use `VKM_PLATFORM_APPLE=ON`.
Also used for **macOS Vulkan** if `VKM_USE_VULKAN_API=ON` on Apple.

## File Conventions

- All implementation files: `.mm` (Objective-C++ — compiled with `-x objective-c++`)
- Headers: `.h` (C++20 with pimpl to hide ObjC types from C++ consumers)
- Guard with `#ifdef VKM_PLATFORM_APPLE`
- Corresponding headers in `include/vkm/platform/apple/`; update both together

## Window Handle

```cpp
// include/vkm/platform/common/window.h
#if defined(VKM_PLATFORM_APPLE)
    #define VKM_WINDOW_HANDLE CAMetalLayer*
#endif
```

On the Metal backend, the window is backed by a `CAMetalLayer` which is passed directly to the Metal swapchain (no GLFW). On the Vulkan backend (`VKM_USE_VULKAN_API`), the window is a GLFW window, same as Linux/Windows.

## pimpl Pattern

The C++ header exposes no ObjC types. Implementation details are hidden via a private implementation struct:

```cpp
// application.h (C++ header — no ObjC)
class VkmApplication {
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// application.mm (ObjC++ implementation)
struct VkmApplication::Impl {
    NSWindow* window;      // macOS
    // UIWindow* window;   // iOS
    CAMetalLayer* layer;
};
```

Do not add `#import <Cocoa/Cocoa.h>` or other ObjC framework imports to `.h` files.

## macOS vs iOS Branching

```objc
#if TARGET_OS_IPHONE
    // iOS: UIApplication, UIWindow, UIViewController
#elif TARGET_OS_OSX
    // macOS: NSApplication, NSWindow, NSViewController
#endif
```

Use `TARGET_OS_*` macros (not `IOS` CMake variable) in source files.

## App Lifecycle

The Apple application drives the event loop via `NSApplicationMain` / `UIApplicationMain`. The engine is created and ticked from the app delegate (`include/vkm/platform/common/app_delegate.h`).

## Input and Window Focus (macOS Metal)

Two windows exist: the scene window (`VkmWindowImpl : NSWindow`) and a plain ImGui window. Only
the scene window forwards input into `VkmEngine::getInputHandler()`; ImGui gets keys and scroll
from an app-wide `NSEvent` local monitor and polls the mouse itself
(`renderer/imgui/metal_imgui_renderer.mm`).

Both windows share `VkmApplicationImpl` as their delegate purely so `windowDidBecomeKey:` /
`windowDidResignKey:` can report focus to `VkmEngine::onWindowFocusChanged`. The engine resolves
the reporting window by comparing against the `NSWindow`s it created and passing the matching
`CAMetalLayer` — the handle `addSwapChain()` was given, and what `findWindowIndex()` matches on.
Deliberately not `contentView.layer`: neither view is marked layer-hosting, so AppKit is free to
substitute a layer there.

That focus index is what stops the ImGui mouse poll from reading the *scene* window's cursor.
`mouseLocationOutsideOfEventStream` answers for whichever window it is asked, with no indication
of where the cursor actually is, so polling a foreign window feeds its coordinates into ImGui's
own `io.DisplaySize` space and raises `WantCaptureMouse` over scene content — which the scene
window's `forwardCursorMoveEvent:` then honours by dropping the event. `VkmImGuiRendererBase::
newFrame(windowFocused)` carries the answer in; `isWindowFocused()` gates the poll.

Losing focus also clears held keys and buttons in `VkmInputHandler`, because the matching release
is delivered to whichever window took focus and would otherwise never arrive.

## Backend Pairing

- macOS/iOS Metal: also read `src/vkm/renderer/backend/metal/AGENTS.md`
- macOS Vulkan: also read `src/vkm/renderer/backend/vulkan/AGENTS.md`

## Implementation Checklist

- [ ] All `.mm` files compile as Objective-C++ (enforced by CMake `COMPILE_FLAGS "-x objective-c++"`)
- [ ] No ObjC imports in `.h` files (pimpl hides them)
- [ ] `CAMetalLayer` pixel format set to match swapchain format before passing to Metal backend
- [ ] `TARGET_OS_IPHONE` / `TARGET_OS_OSX` used for platform branching
- [x] App delegate properly forwards resize events to engine (`windowWillStartLiveResize:` / `windowDidEndLiveResize:` / `windowDidResize:` / `windowDidChangeBackingProperties:`)
- [ ] `autorelease` pool created around the main event loop if not using `NSApplicationMain`
