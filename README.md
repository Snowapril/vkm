# vkm

[![CodeFactor](https://www.codefactor.io/repository/github/snowapril/vkm/badge)](https://www.codefactor.io/repository/github/snowapril/vkm)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/c2815435af9745a78831c15659dd996a)](https://www.codacy.com/gh/snowapril/vkm/dashboard?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=snowapril/vkm&amp;utm_campaign=Badge_Grade)
![Ubuntu github action](https://github.com/snowapril/vkm/actions/workflows/ubuntu.yml/badge.svg?branch=main)
![Window github action](https://github.com/snowapril/vkm/actions/workflows/window.yml/badge.svg?branch=main)
![MacOS github action](https://github.com/snowapril/vkm/actions/workflows/macos.yml/badge.svg?branch=main)

## QuickStart
```bash
git clone https://github.com/snowapril/vkm.git
cd vkm
python bootstrap.py
mkdir build
cd build
cmake .. && cmake --build .
```

## Dependencies
*   [vulkan-hpp](https://github.com/KhronosGroup/Vulkan-Hpp)
*   [glfw](https://github.com/glfw/glfw)
*   [glm](https://github.com/g-truc/glm)
*   [imgui](https://github.com/ocornut/imgui)
*   [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo)
*   [stb](https://github.com/nothings/stb)
*   [taskflow](https://github.com/taskflow/taskflow)
*   [meshoptimizer](https://github.com/zeux/meshoptimizer)
*   [glslang](https://github.com/KhronosGroup/glslang)
*   [doctest](https://github.com/doctest/doctest)

## Running Unit Tests

After running `bootstrap.py`, use the platform-appropriate runner script.

**macOS / Linux**
```bash
# Default backend (Metal on macOS, Vulkan on Linux)
bash scripts/run_tests.sh

# Explicit backend
bash scripts/run_tests.sh --backend metal
bash scripts/run_tests.sh --backend vulkan
bash scripts/run_tests.sh --backend webgpu

# All supported backends on the current platform
bash scripts/run_tests.sh --backend all

# Release build
bash scripts/run_tests.sh --build-type Release
```
`run_tests.sh` uses associative arrays and needs bash 4+; macOS ships bash 3.2, so install a
newer one first (e.g. `brew install bash`) and invoke it explicitly if `bash --version` shows
3.x (`$(brew --prefix)/bin/bash scripts/run_tests.sh ...`).

**Windows (PowerShell)**
```powershell
.\scripts\run_tests.ps1
.\scripts\run_tests.ps1 -BuildType Release
```

Each backend is built in its own isolated directory (`build/metal/`, `build/vulkan/`,
`build/webgpu/`). Pass `--no-deps-check` / `-NoDepsCheck` to skip the `dependencies/src/`
existence check if you have already run the bootstrap script.

The **webgpu** backend targets Emscripten/WASM and is executed headlessly in a local Chrome
or Chromium install (Node.js can't run it — it lacks `navigator.gpu`). It requires the emsdk
toolchain, which `bootstrap.py` installs into `dependencies/src/emsdk`; if either emsdk or
Chrome/Chromium isn't found, it's skipped with an informational message rather than failing.

## Running Samples

After running `bootstrap.py`, use `scripts/run_sample.*` to configure, build, and run any
sample for a given backend in one step.

**macOS / Linux**
```bash
bash scripts/run_sample.sh --backend metal --sample triangle
bash scripts/run_sample.sh --backend vulkan --sample triangle
bash scripts/run_sample.sh --backend webgpu --sample triangle
```

**Windows (PowerShell)**
```powershell
.\scripts\run_sample.ps1 -Backend vulkan -Sample triangle
.\scripts\run_sample.ps1 -Backend webgpu -Sample triangle
```

Native backends (`metal`, `vulkan`) build into `build/<backend>/` and launch the sample
directly as a normal GUI window. The `webgpu` backend builds into `build-wasm/` (matching
the manual WebGPU instructions below) and opens the sample in your default browser via a
short-lived local HTTP server. See "Native Backends" and "WebGPU" below for the equivalent
manual, step-by-step commands each of these scripts automates.

## Native Backends (Metal / Vulkan)

Metal and Vulkan are both built directly by CMake (no browser/emsdk step). Metal is available
on Apple platforms only and requires the Metal 4 (`MTL4*`) headers, which ship starting with
the macOS 26 SDK — older Xcode/SDK installs will not have them. Vulkan is available on Linux
and Windows natively, and on macOS via MoltenVK; it requires a Vulkan loader/SDK to be
discoverable (either the `VULKAN_SDK` environment variable set, or `vulkaninfo` on `PATH`) —
install the [LunarG Vulkan SDK](https://vulkan.lunarg.com/) if you don't already have one. No
separate setup step is needed beyond `python bootstrap.py`.

> **Known limitation:** building the `triangle` sample with the Vulkan backend on macOS
> currently fails to link. The Apple platform windowing/render-coordinator code
> (`src/vkm/platform/apple/application.mm`) references the Metal-specific driver/swapchain
> types directly, regardless of which backend is selected, so a Vulkan-only build never
> compiles those types in. This doesn't affect Vulkan on Linux/Windows, or the `UnitTests`
> target on macOS (it doesn't create a window). Use Metal for the sample on macOS until
> `application.mm` is updated to dispatch by backend.

**Building the sample manually (Metal)**
```bash
cmake -B build/metal -DCMAKE_BUILD_TYPE=Release \
  -DVKM_USE_METAL_API=ON -DVKM_USE_VULKAN_API=OFF -DBUILD_SAMPLES=ON
cmake --build build/metal --target triangle
```

**Building the sample manually (Vulkan)**
```bash
cmake -B build/vulkan -DCMAKE_BUILD_TYPE=Release \
  -DVKM_USE_VULKAN_API=ON -DVKM_USE_METAL_API=OFF -DBUILD_SAMPLES=ON
cmake --build build/vulkan --target triangle
```

**Running it**
```bash
./build/metal/bin/triangle    # Metal
./build/vulkan/bin/triangle   # Vulkan (triangle.exe on Windows)
```

For the fully-automated path, prefer `scripts/run_sample.py --backend metal --sample triangle`
(or `--backend vulkan`, or `run_sample.sh`/`run_sample.ps1`, see "Running Samples" above) over
the manual steps here.

## WebGPU (Emscripten/WASM)

`python bootstrap.py` already installs and pins the emsdk toolchain into
`dependencies/src/emsdk` — no separate setup step needed. The only other prerequisite is a
local Chrome or Chromium install.

**Building the sample manually**
```bash
source dependencies/src/emsdk/emsdk_env.sh   # emsdk_env.bat on Windows
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --target triangle
```

**Running it in a browser** — serve the output directory and open it in Chrome:
```bash
cd build-wasm/bin
python3 -m http.server 8080
# open http://localhost:8080/triangle.html in Chrome
```
A normal (non-headless) Chrome window has WebGPU available by default on supported platforms
— the `--enable-unsafe-webgpu --use-angle=swiftshader` flags below are a headless/software-rendering
concern, not something you need for a manual, GUI browser check.

**Building/running the WebGPU unit tests manually** — the same `emcmake`/`cmake --build --target UnitTests`
sequence produces `build-wasm/bin/UnitTests.html`. For interactive debugging, just open it in a
normal Chrome window and watch doctest's output in DevTools. To reproduce exactly what the
automated runner (`scripts/run_tests.py --backend webgpu`, see above) does headlessly:
```bash
google-chrome --headless=new --disable-gpu-sandbox --enable-unsafe-webgpu \
  --enable-features=Vulkan --use-angle=swiftshader --enable-logging=stderr --v=1 \
  http://localhost:8080/UnitTests.html
```
(macOS: use `/Applications/Google\ Chrome.app/Contents/MacOS/Google\ Chrome` in place of `google-chrome`.)

For the fully-automated path, prefer `scripts/run_tests.py --backend webgpu` (or `run_tests.sh`/`run_tests.ps1`,
see "Running Unit Tests" above) over the manual steps here — it also handles the local HTTP server for you.

For the fully-automated **sample** path (as opposed to tests), use
`scripts/run_sample.py --backend webgpu --sample triangle` (or `run_sample.sh`/`run_sample.ps1`,
see "Running Samples" above) — it builds via `emcmake`, serves `build-wasm/bin` locally, and
opens `triangle.html` directly in your default browser.

## How To Contribute

Contributions are always welcome, either reporting issues/bugs or forking the repository and then issuing pull requests when you have completed some additional coding that you feel will be beneficial to the main project. If you are interested in contributing in a more dedicated capacity, then please contact me.

## Contact

You can contact me via e-mail (sinjihng at gmail.com). I am always happy to answer questions or help with any issues you might have, and please be sure to share any additional work or your creations with me, I love seeing what other people are making.

## License

<img align="right" src="http://opensource.org/trademarks/opensource/OSI-Approved-License-100x137.png">

The class is licensed under the [MIT License](http://opensource.org/licenses/MIT):

Copyright (c) 2024 Snowapril

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
