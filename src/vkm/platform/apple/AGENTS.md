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

## Backend Pairing

- macOS/iOS Metal: also read `src/vkm/renderer/backend/metal/AGENTS.md`
- macOS Vulkan: also read `src/vkm/renderer/backend/vulkan/AGENTS.md`

## Implementation Checklist

- [ ] All `.mm` files compile as Objective-C++ (enforced by CMake `COMPILE_FLAGS "-x objective-c++"`)
- [ ] No ObjC imports in `.h` files (pimpl hides them)
- [ ] `CAMetalLayer` pixel format set to match swapchain format before passing to Metal backend
- [ ] `TARGET_OS_IPHONE` / `TARGET_OS_OSX` used for platform branching
- [ ] App delegate properly forwards resize events to engine
- [ ] `autorelease` pool created around the main event loop if not using `NSApplicationMain`
