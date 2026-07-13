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
    id<MTLDevice> _mtlDevice{nullptr};  // ARC-managed
};

// In .mm (implementation)
#import <Metal/Metal.h>
```

**Default-initialize `id<Protocol>` members with `nullptr`, not `nil`, in `.h` files.** `nil` is
a macro defined by the Objective-C runtime headers, which a pure forward-declaring header does
not import — `id<MTLBuffer> _mtlBuffer{nil};` fails to compile with "use of undeclared identifier
'nil'" outside a `.mm` file. `nullptr` is a real C++ keyword and works identically here.

## Memory Allocation

Committed vs. pooled placement mirrors the Vulkan side's thresholds/flags (see
`vulkan/AGENTS.md`), decided per-resource inside each concrete class's own `initialize()`.
Committed = today's direct `[device newBufferWithLength:options:]` /
`newTextureWithDescriptor:]` path. Pooled buffers come from `VkmGpuHeapPoolMetal`, one
`MTLHeapTypeAutomatic` heap per 64 MiB block, owned by `VkmDriverMetal`. Unlike Vulkan's
manual `VkmOffsetAllocator`-based pool, **freeing a pooled Metal buffer needs no explicit
release call** — dropping the ARC-managed `id<MTLBuffer>` reference lets the heap reclaim
that space internally. `MTLHeapTypePlacement` (manual caller-managed offsets) is deliberately
not used — `MTLHeapTypeAutomatic` already does the placement work `VkmGpuHeapPoolMetal` would
otherwise have to hand-roll, and this project has no need for the additional control
`MTLHeapTypePlacement` offers. `VkmSamplerMetal` has no memory backing at all (mirrors Vulkan's
`VkSampler`). `VkmStagingBufferMetal` is always committed + `MTLStorageModeShared` (persistently
host-visible; no explicit map/unmap step exists in the Metal API at all).

## Class Override Map

### VkmDriverMetal
Overrides all 7 `VkmDriverBase` pure virtuals:
- `initializeInner` — MTLCreateSystemDefaultDevice, store `_mtlDevice`
- `destroyInner` — release Metal resources
- `newTextureInner` → `new VkmTextureMetal`
- `newBufferInner` → `new VkmBufferMetal`
- `newStagingBufferInner` → `new VkmStagingBufferMetal`
- `newSamplerInner` → `new VkmSamplerMetal`
- `newTextureViewInner` → `new VkmTextureViewMetal`
- `newBufferViewInner` → `new VkmBufferViewMetal` (metadata-only; Metal has no buffer-view object)
- `newSwapChainInner` → `new VkmSwapChainMetal`
- `newCommandQueueInner` → `new VkmCommandQueueMetal` (via `VkmCommandBufferPoolMetal`)
- `newRenderResourcePoolInner` → `new VkmRenderResourcePoolMetal`

### VkmRenderResourcePoolMetal
- Owns one `id<MTLResidencySet>` per `VkmResourcePoolType` (only `Default` today), attached to each `MTL4CommandQueue` at queue init — Metal4 command buffers do not implicitly make referenced resources resident
- Sets are created in `initialize()` (called after `initializeInner()`'s device validation), never in the constructor — residency-set APIs hang inside the Metal framework on unsupported (e.g. paravirtualized CI) GPUs instead of returning nil
- `onResourceInitialized` / `releaseResource` — stage `addAllocation:`/`removeAllocation:` for Buffer/Texture/StagingBuffer native handles (samplers/views own no distinct `MTLAllocation`); guarded by a dedicated mutex because the deferred reclaimer releases from a background thread
- `commitPendingResidencyChanges` — flushes staged changes with one `commit` per set; called from `VkmCommandQueueMetal::submit()` before command-buffer commit, no-op when nothing staged
- `registerExternalAllocation` — residency for allocations outside handle tracking (e.g. `MTLHeap` pool blocks, registered in `allocateFromHeapPool`)
- Swapchain backbuffers are exempt: they bypass `newTexture()` and swap their drawable-provided native handle every frame

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

Minimum target: macOS 26.0 (`CMAKE_OSX_DEPLOYMENT_TARGET "26.0"`), iOS 26+. Non-Apple Silicon and pre-26 OS versions are not supported.

`VkmDriverMetal::initializeInner()` enforces this at runtime:
1. Nil device check → error + return false
2. `@available(macOS 26.0, iOS 26.0, *)` OS version check → error + return false if not met
3. `[device supportsFamily:MTLGPUFamilyApple9]` GPU family check → error + return false if not met

```objc
[device supportsFamily:MTLGPUFamilyApple9]  // Metal4 minimum GPU family
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
- [ ] `id<Protocol>` members default-initialized with `nullptr` in headers, never `nil`
- [ ] Encoder ended (`endEncoding`) before committing command buffer
- [ ] `TARGET_OS_IPHONE` / `TARGET_OS_OSX` used for platform branching (not `IOS` macro)
- [ ] Shared event signaled after command buffer commit in `VkmCommandQueueMetal::submit`
- [ ] ARC enabled (no manual `retain`/`release` in `.mm` files)
