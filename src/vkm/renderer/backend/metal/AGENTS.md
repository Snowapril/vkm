# Metal Backend

Targets: **macOS Metal4** (`IOS=OFF`) and **iOS Metal4** (`IOS=ON`). Both use `VKM_USE_METAL_API` + `VKM_PLATFORM_APPLE`.

## File Conventions

- Headers: `.h` (C++20 with forward-declared ObjC protocols using `@protocol`)
- Implementations: `.mm` (Objective-C++ — mandatory, compiled with `-x objective-c++`)
- Never put Metal ObjC types (`id<MTL*>`) in `.cpp` files
- Guard all code with `#ifdef VKM_USE_METAL_API`
- Corresponding headers live in `include/vkm/renderer/backend/metal/`; update both together

## Linked Frameworks

```
Metal, CoreVideo, QuartzCore (all Apple platforms)
Cocoa, AppKit (macOS only — not linked on iOS; see `src/vkm/CMakeLists.txt`)
```
These are linked in `src/vkm/CMakeLists.txt` for `VKM_USE_METAL_API`. Do not add system framework `#import` in headers exposed to pure C++ translation units.

## ObjC Type Rules

- Use ARC (`id<MTL*>` types are ARC-managed in `.mm` files)
- In C++ class members, store Metal objects as `id<Protocol>` (ObjC pointer, ARC-managed)
- Forward-declare protocols in headers: `@protocol MTLDevice;` avoids importing Metal.h in C++ headers

```cpp
// In .h (C++ header)
@protocol MTLDevice;
class VkmDriverMetal : public VkmDriverBase {
    id<MTLDevice> _mtlDevice;  // ARC-managed
};

// In .mm (implementation)
#import <Metal/Metal.h>
```

## Class Override Map

### VkmDriverMetal
Overrides all 5 `VkmDriverBase` pure virtuals:
- `initializeInner` — MTLCreateSystemDefaultDevice, store `_mtlDevice`
- `destroyInner` — release Metal resources
- `newTextureInner` → `new VkmTextureMetal`
- `newSwapChainInner` → `new VkmSwapChainMetal`
- `newCommandQueueInner` → `new VkmCommandQueueMetal` (via `VkmCommandBufferPoolMetal`)

### VkmCommandQueueMetal
- Holds `id<MTLCommandQueue> _mtlCommandQueue`
- `initializeInner` — cast `_driver` to `VkmDriverMetal*`, call `getMTLDevice()`, then `[device newCommandQueue]`; also creates `VkmCommandBufferPoolMetal` and `VkmGpuEventTimelineMetal`
- `submit(CommandSubmitInfo)` — commits all pending `VkmCommandBufferBase*` entries, signals `VkmGpuEventTimelineMetal`

### VkmGpuEventTimelineMetal
- Wraps `id<MTLSharedEvent>` for GPU/CPU sync
- `queryLastCompletedTimeline` — reads signaled value from shared event
- `waitIdle(timeoutMs)` — blocks CPU until GPU timeline reaches current value

### VkmCommandBufferPoolMetal
- `getOrCreateRHICommandBuffer` — dequeues or creates `id<MTLCommandBuffer>` from `_mtlCommandQueue`
- `newCommandBuffer` → `new VkmCommandBufferMetal`

## macOS vs iOS Branching

```objc
#if TARGET_OS_IPHONE
    // iOS path
#elif TARGET_OS_OSX
    // macOS path
#endif
```
Do not use `#ifdef IOS` in `.mm` files — use Apple's standard `TARGET_OS_*` macros.

## Metal4 GPU Family

Minimum target: macOS 14.5 (`CMAKE_OSX_DEPLOYMENT_TARGET "14.5"`), iOS 18+.
When checking GPU capabilities use:
```objc
[device supportsFamily:MTLGPUFamilyApple9]  // Metal4 feature set
```

## Command Encoder Lifecycle

Metal encoders are single-use. The pattern in `VkmCommandBufferMetal`:
1. Begin render pass → create `MTLRenderCommandEncoder`
2. Record commands → call encoder methods
3. End render pass → `[encoder endEncoding]`, set encoder to nil
4. Never reuse an ended encoder

## Implementation Checklist

- [ ] All `.mm` files import `<Metal/Metal.h>` (not in headers)
- [ ] No Metal types in `.h` files exposed to pure C++ (use `@protocol` forward declarations)
- [ ] Encoder ended (`endEncoding`) before committing command buffer
- [ ] `TARGET_OS_IPHONE` / `TARGET_OS_OSX` used for platform branching (not `IOS` macro)
- [ ] Shared event signaled after command buffer commit in `VkmCommandQueueMetal::submit`
- [ ] ARC enabled (no manual `retain`/`release` in `.mm` files)
