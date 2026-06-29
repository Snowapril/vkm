# vkm — Cross-Platform Renderer

C++20, CMake 3.18.6+. Supports 4 platform/API combinations:

| Combination | CMake Flags | File Extensions | Key Dependencies |
|---|---|---|---|
| Windows Vulkan | `VKM_PLATFORM_WINDOWS=ON` `VKM_USE_VULKAN_API=ON` | `.h` `.cpp` | volk, VMA, GLFW |
| macOS Vulkan | `VKM_PLATFORM_APPLE=ON` `VKM_USE_VULKAN_API=ON` | `.h` `.cpp` | volk, VMA |
| macOS Metal4 | `VKM_PLATFORM_APPLE=ON` `VKM_USE_METAL_API=ON` `IOS=OFF` | `.h` `.mm` | Metal, Cocoa, QuartzCore |
| iOS Metal4 | `VKM_PLATFORM_APPLE=ON` `VKM_USE_METAL_API=ON` `IOS=ON` | `.h` `.mm` | Metal, UIKit, QuartzCore |

## Architecture (3 Layers)

```
common/          ← abstract interfaces (VkmDriverBase, VkmSwapChainBase, VkmCommandBufferBase)
vulkan/ metal/   ← API-specific implementations
platform/        ← windowing (windows/ = GLFW, apple/ = CAMetalLayer)
```

Each backend implements the abstract interface. Never modify public methods in `common/`; only override `protected virtual` methods in the backend subclasses.

## Build Commands

```bash
# macOS Metal4 (default on macOS)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# macOS Metal4 Release
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# iOS Metal4 (Xcode)
cmake -B build-ios -DIOS=ON -G Xcode
xcodebuild -project build-ios/vkm.xcodeproj -configuration Debug

# Windows Vulkan (on Windows with Vulkan SDK)
cmake -B build -DVKM_USE_VULKAN_API=ON -DVKM_PLATFORM_WINDOWS=ON
cmake --build build
```

## Directory → AGENTS.md Routing

| Working on... | Read this AGENTS.md |
|---|---|
| Common interfaces / contracts | `include/vkm/renderer/backend/common/AGENTS.md` |
| Vulkan backend (Windows or macOS) | `src/vkm/renderer/backend/vulkan/AGENTS.md` |
| Metal backend (macOS or iOS) | `src/vkm/renderer/backend/metal/AGENTS.md` |
| Windows platform layer | `src/vkm/platform/windows/AGENTS.md` |
| macOS/iOS platform layer | `src/vkm/platform/apple/AGENTS.md` |

## Global Rules

- All source code, comments, identifiers: **English only**
- No speculative features — implement only what is asked
- Match existing style; do not reformat unrelated code
- `include/` mirrors `src/` — always update both header and implementation together
- Build definitions (`VKM_USE_VULKAN_API`, `VKM_USE_METAL_API`, etc.) control which files compile; guard platform-specific code with the corresponding `#ifdef`
