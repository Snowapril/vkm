# vkm — Cross-Platform Renderer

C++20, CMake 3.18.6+. Supports 5 platform/API combinations:

| Combination | CMake Flags | File Extensions | Key Dependencies |
|---|---|---|---|
| Windows Vulkan | `VKM_PLATFORM_WINDOWS=ON` `VKM_USE_VULKAN_API=ON` | `.h` `.cpp` | volk, VMA, GLFW |
| macOS Vulkan | `VKM_PLATFORM_APPLE=ON` `VKM_USE_VULKAN_API=ON` | `.h` `.cpp` | volk, VMA |
| macOS Metal4 | `VKM_PLATFORM_APPLE=ON` `VKM_USE_METAL_API=ON` `IOS=OFF` | `.h` `.mm` | Metal, Cocoa, QuartzCore |
| iOS Metal4 | `VKM_PLATFORM_APPLE=ON` `VKM_USE_METAL_API=ON` `IOS=ON` | `.h` `.mm` | Metal, UIKit, QuartzCore |
| Emscripten/WASM WebGPU | `VKM_PLATFORM_WASM=ON` `VKM_USE_WEBGPU_API=ON` | `.h` `.cpp` | emdawnwebgpu (Emscripten port), GLFW (via `-sUSE_GLFW=3`) |

The Emscripten/WASM combination targets a Chromium-based browser only (no native desktop WebGPU); it is compiled from an Ubuntu, macOS, or Windows host via emsdk — the host OS is not the runtime target.

## Architecture (3 Layers)

```
common/                  ← abstract interfaces (VkmDriverBase, VkmSwapChainBase, VkmCommandBufferBase)
vulkan/ metal/ webgpu/   ← API-specific implementations
platform/                ← windowing (windows/ = GLFW, apple/ = CAMetalLayer, wasm/ = GLFW via Emscripten port)
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

# Emscripten/WASM WebGPU (on Ubuntu, macOS, or Windows with emsdk activated)
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --target triangle
```

## Emscripten/WASM Toolchain Setup

`bootstrap.py` clones `emsdk` into `dependencies/src/emsdk` and installs/activates the pinned release (`6.0.2`, see `dependencies/bootstrap.json` and `dependencies/patches/emsdk_install.py`). After bootstrapping, activate it for the current shell:

```bash
source dependencies/src/emsdk/emsdk_env.sh   # emsdk_env.bat on Windows
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --target triangle
```

## Directory → AGENTS.md Routing

| Working on... | Read this AGENTS.md |
|---|---|
| Common interfaces / contracts | `include/vkm/renderer/backend/common/AGENTS.md` |
| Vulkan backend (Windows or macOS) | `src/vkm/renderer/backend/vulkan/AGENTS.md` |
| Metal backend (macOS or iOS) | `src/vkm/renderer/backend/metal/AGENTS.md` |
| WebGPU backend (Emscripten/WASM) | `src/vkm/renderer/backend/webgpu/AGENTS.md` |
| Windows platform layer | `src/vkm/platform/windows/AGENTS.md` |
| macOS/iOS platform layer | `src/vkm/platform/apple/AGENTS.md` |
| WASM platform layer | `src/vkm/platform/wasm/AGENTS.md` |

## Global Rules

- All source code, comments, identifiers: **English only**
- No speculative features — implement only what is asked
- Match existing style; do not reformat unrelated code
- `include/` mirrors `src/` — always update both header and implementation together
- Build definitions (`VKM_USE_VULKAN_API`, `VKM_USE_METAL_API`, etc.) control which files compile; guard platform-specific code with the corresponding `#ifdef`
