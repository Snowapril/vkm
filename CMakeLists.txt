# CMake version
cmake_minimum_required(VERSION 3.18.6 FATAL_ERROR)
cmake_policy(SET CMP0063 NEW) # visibility
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED 20)

if (APPLE AND NOT IOS)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum OS X deployment version")
endif()

# Include cmake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/buildscripts")

# Declare project
project(vkm)

# Set output directories
set(DEFAULT_CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)

# Multi-config generators (Visual Studio, Xcode) append a per-config subfolder
# (e.g. bin/Release/) unless the per-config variants below are also set — without
# this, scripts/run_tests.py's fixed bin/UnitTests(.exe) path never finds the binary
# on Windows, where the default generator is multi-config.
foreach (OUTPUTCONFIG IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${PROJECT_BINARY_DIR}/bin)
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${PROJECT_BINARY_DIR}/lib)
	set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${PROJECT_BINARY_DIR}/lib)
endforeach ()

# Set enable output of compile commands during generation
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

# Compile options
include(buildscripts/CompileOptions.cmake)
include(buildscripts/ShaderCompile.cmake)

# Build type - Release by default
message("CMake build type: " ${CMAKE_BUILD_TYPE})
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
elseif(CMAKE_BUILD_TYPE MATCHES "Debug")
    add_definitions(-DVKM_DEBUG)
endif()

if (UNIX AND NOT APPLE AND NOT ANDROID AND NOT WEBGL AND NOT EMSCRIPTEN)
    set(LINUX TRUE)
else()
    # since cmake 3.25 LINUX is automatically set based on CMAKE_SYSTEM_NAME, which the android
    # cmake files are setting to "Linux".
    set(LINUX FALSE)
endif()

if (LINUX OR WIN32)
    option(VKM_USE_VULKAN_API "Use Vulkan API" ON)
else()
    option(VKM_USE_VULKAN_API "Use Vulkan API" OFF)
endif()

if (WIN32)
    option(VKM_PLATFORM_WINDOWS "Build target device is windows platform" ON)
else()
    option(VKM_PLATFORM_WINDOWS "Build target device is windows platform" OFF)
endif()

if (LINUX)
    option(VKM_PLATFORM_LINUX "Build target device is linux platform" ON)
else()
    option(VKM_PLATFORM_LINUX "Build target device is linux platform" OFF)
endif()

if (APPLE)
    option(VKM_USE_METAL_API "Use Metal API on macOS/iOS" ON)
    option(VKM_PLATFORM_APPLE "Build target device is apple platform" ON)
else()
    option(VKM_USE_METAL_API "Use Metal API on macOS/iOS" OFF)
    option(VKM_PLATFORM_APPLE "Build target device is apple platform" OFF)
endif()

if (EMSCRIPTEN)
    option(VKM_USE_WEBGPU_API "Use WebGPU API (Emscripten/WASM only)" ON)
    option(VKM_PLATFORM_WASM "Build target device is Emscripten/WASM platform" ON)
else()
    option(VKM_USE_WEBGPU_API "Use WebGPU API (Emscripten/WASM only)" OFF)
    option(VKM_PLATFORM_WASM "Build target device is Emscripten/WASM platform" OFF)
endif()

if (VKM_USE_VULKAN_API)
    add_definitions(-DVKM_USE_VULKAN_API)
endif()

if (VKM_USE_METAL_API)
    add_definitions(-DVKM_USE_METAL_API)
endif()

if (VKM_USE_WEBGPU_API)
    add_definitions(-DVKM_USE_WEBGPU_API)
endif()

if (VKM_PLATFORM_WINDOWS)
    add_definitions(-DVKM_PLATFORM_WINDOWS)
endif()

if (VKM_PLATFORM_LINUX)
    add_definitions(-DVKM_PLATFORM_LINUX)
endif()

if (VKM_PLATFORM_APPLE)
    add_definitions(-DVKM_PLATFORM_APPLE)
endif()

if (VKM_PLATFORM_WASM)
    add_definitions(-DVKM_PLATFORM_WASM)
endif()

if (EMSCRIPTEN)
    # Emscripten doesn't support C++ exceptions by default (no -fexceptions passed);
    # tell spdlog's bundled fmt not to rely on try/catch.
    add_definitions(-DSPDLOG_NO_EXCEPTIONS)
endif()

if (EMSCRIPTEN OR VKM_PLATFORM_APPLE)
    # Some Emscripten clang versions and newer AppleClang (macOS 26 SDK toolchain) fail to
    # evaluate FMT_STRING's compile-time format-string check as a constant expression ("call
    # to consteval function ... is not a constant expression"), even though they otherwise
    # report consteval support. spdlog's bundled fmt (spdlog/fmt/bundled/base.h) already
    # honors an externally-provided FMT_USE_CONSTEVAL definition before running its own
    # auto-detection, so predefine it here to force the non-consteval (runtime) path on
    # these targets. This used to be a source patch applied to the vendored spdlog checkout,
    # but that broke whenever bootstrap.py fetched a newer spdlog/fmt with different
    # surrounding context; a compile definition doesn't depend on the vendored source's
    # exact text and can't drift out of sync with it.
    add_definitions(-DFMT_USE_CONSTEVAL=0)
endif()

if (EMSCRIPTEN)
    # Emscripten emits a companion .js/.wasm alongside the .html shell.
    set(CMAKE_EXECUTABLE_SUFFIX ".html")
endif()

if (VKM_USE_WEBGPU_API)
    # Single source of truth for Emscripten port/link flags: vkmcore and the
    # sample executables must be compiled/linked with identical port settings
    # or emcc fails to link ("mixing ports compiled with different settings").
    # --use-port must also be passed at compile time (it controls header search
    # paths); the -s linker settings below are rejected by emcc during compilation.
    set(VKM_EMSCRIPTEN_COMPILE_FLAGS
        "SHELL:--use-port=emdawnwebgpu"
    )
    set(VKM_EMSCRIPTEN_PORT_FLAGS
        "SHELL:--use-port=emdawnwebgpu"
        "SHELL:-s USE_GLFW=3"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s ASYNCIFY"
    )
endif()

# Build Options
option(BUILD_COVERAGE "Build code coverage" OFF)
option(GPU_RESOURCE_DEBUG_NAME "Enable vulkan object debug name" ON)
# Default ON except for Release builds; pass -DGPU_BREAD_CRUMBS=ON to opt a Release build back in.
if (CMAKE_BUILD_TYPE MATCHES Release)
    set(VKM_GPU_BREAD_CRUMBS_DEFAULT OFF)
else()
    set(VKM_GPU_BREAD_CRUMBS_DEFAULT ON)
endif()
option(GPU_BREAD_CRUMBS "Enable GPU crash breadcrumb instrumentation (submission history + per-subgraph completion markers)" ${VKM_GPU_BREAD_CRUMBS_DEFAULT})
# Default ON except for Release builds; pass -DGPU_CAPTURE=ON to opt a Release build back in.
if (CMAKE_BUILD_TYPE MATCHES Release)
    set(VKM_GPU_CAPTURE_DEFAULT OFF)
else()
    set(VKM_GPU_CAPTURE_DEFAULT ON)
endif()
option(GPU_CAPTURE "Enable programmatic GPU frame capture (Metal MTLCaptureScope + .gputrace export)" ${VKM_GPU_CAPTURE_DEFAULT})
# Metal only: keep the intermediate .metal source beside each .vfcache and embed it
# in the metallib (-frecord-sources) so Xcode GPU captures show shader source. Default
# ON except for Release builds (leaner caches); pass -DVKM_METAL_EMIT_MSL_SOURCE=ON to opt in.
if (CMAKE_BUILD_TYPE MATCHES Release)
    set(VKM_METAL_EMIT_MSL_SOURCE_DEFAULT OFF)
else()
    set(VKM_METAL_EMIT_MSL_SOURCE_DEFAULT ON)
endif()
option(VKM_METAL_EMIT_MSL_SOURCE "Emit intermediate MSL source + embed it in metallib for shader debugging" ${VKM_METAL_EMIT_MSL_SOURCE_DEFAULT})
option(CHROME_TRACING "Enable Chrome Tracing for CPU profiling" ON)
option(TASKFLOW_PROFILER, "Enable Taskflow Profiler" ON)
option(VKM_ENABLE_IMGUI "Enable ImGui integration" ON)

if (CMAKE_BUILD_TYPE MATCHES Debug AND CMAKE_COMPILER_IS_GNUCXX AND BUILD_COVERAGE)
    include(CodeCoverage)
    setup_target_for_coverage(${PROJECT_NAME}_coverage UnitTests coverage)
endif()

if (GPU_RESOURCE_DEBUG_NAME)
    add_definitions(-DVKM_DEBUG_NAME_ENABLED)
endif()

if (GPU_BREAD_CRUMBS)
    add_definitions(-DVKM_ENABLE_GPU_BREAD_CRUMBS)
endif()

if (GPU_CAPTURE)
    add_definitions(-DVKM_GPU_CAPTURE)
endif()

if (CHROME_TRACING)
    add_definitions(-DENABLE_CHROME_TRACING)
endif()

if (TASKFLOW_PROFILER)
    # You can visualize profiled json file at https://taskflow.github.io/tfprof/ 
    add_definitions(-DTF_ENABLE_PROFILER=taskflow_profile.json)
endif()

if (VKM_ENABLE_IMGUI)
    add_definitions(-DVKM_ENABLE_IMGUI)
endif()

# Overrides
set(CMAKE_MACOSX_RPATH ON)

if (APPLE AND CMAKE_GENERATOR STREQUAL "Xcode")
    # Xcode's default GPU Frame Capture mode ("Automatically", used when the scheme has
    # no explicit setting) often fails to offer Metal capture for plain command-line
    # executables like the samples/tests. Pin the generated schemes to "Metal" so the
    # Capture GPU Workload button is always available when running from Xcode.
    set(CMAKE_XCODE_SCHEME_ENABLE_GPU_FRAME_CAPTURE_MODE "Metal")
endif()

# Set resource directory
set(RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/")

# Add definitions for third-party dependencies
add_definitions(-DGLM_FORCE_SWIZZLE)
add_definitions(-DGLM_FORCE_RADIANS)
add_definitions(-DGLM_FORCE_CTOR_INIT)
add_definitions(-DGLM_ENABLE_EXPERIMENTAL)

# Includes
include_directories(dependencies/src)
include_directories(dependencies/src/vulkan/include)
include_directories(dependencies/src/backward-cpp)
include_directories(dependencies/src/vma/src)
include_directories(dependencies/src/glfw/include)
include_directories(dependencies/src/glm)
include_directories(dependencies/src/glslang/glslang/Include)
include_directories(dependencies/src/taskflow)
include_directories(dependencies/src/stb)
include_directories(dependencies/src/offsetAllocator)
include_directories(dependencies/src/glslang)
include_directories(dependencies/src/glslang/StandAlone)
include_directories(dependencies/src/glslang/src)
include_directories(dependencies/src/ImGuizmo/src)
include_directories(dependencies/src/spdlog/include)
include_directories(dependencies/src/json/include)

if (NOT EMSCRIPTEN)
    set(VMA_BUILD_SAMPLE                    OFF CACHE BOOL "")
    set(VMA_BUILD_SAMPLE_SHADERS            OFF CACHE BOOL "")
    set(VMA_STATIC_VULKAN_FUNCTIONS         OFF CACHE BOOL "")
    set(VMA_DYNAMIC_VULKAN_FUNCTIONS         ON CACHE BOOL "")
    set(VMA_DEBUG_INITIALIZE_ALLOCATIONS     ON CACHE BOOL "")
    set(VMA_RECORDING_ENABLED                ON CACHE BOOL "")
    add_subdirectory(dependencies/src/vma)
endif()

if (NOT EMSCRIPTEN)
    # mimalloc has no official Emscripten/WASM CMake target; memory.cpp falls
    # back to plain ::operator new/delete on VKM_PLATFORM_WASM instead.
    set(MI_BUILD_SHARED OFF CACHE BOOL "")
    set(MI_BUILD_STATIC ON  CACHE BOOL "")
    set(MI_BUILD_TESTS  OFF CACHE BOOL "")
    # We provide our own explicit tracked operator new/delete override in
    # src/vkm/base/memory.cpp; mimalloc's own malloc-interface override would
    # bypass that tagging/tracking layer entirely.
    set(MI_OVERRIDE     OFF CACHE BOOL "")
    add_subdirectory(dependencies/src/mimalloc)
endif()

if (NOT EMSCRIPTEN)
    # Emscripten supplies its own GLFW3 emulation via -sUSE_GLFW=3; the
    # vendored GLFW does not support Emscripten as a CMake build target.
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "")
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "")
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "")
    set(GLFW_INSTALL        OFF CACHE BOOL "")
    add_subdirectory(dependencies/src/glfw)
endif()

set(MESHOPT_BUILD_DEMO          OFF CACHE BOOL "")
set(MESHOPT_BUILD_TOOLS         OFF CACHE BOOL "")
set(MESHOPT_BUILD_SHARED_LIBS   OFF CACHE BOOL "")
add_subdirectory(dependencies/src/meshoptimizer)

set(ENABLE_GLSLANG_BINARIES  OFF  CACHE BOOL "")
set(ENABLE_HLSL              OFF  CACHE BOOL "")
set(ENABLE_CTEST             OFF  CACHE BOOL "")
set(ENABLE_OPT               OFF  CACHE BOOL "")
set(SKIP_GLSLANG_INSTALL      ON  CACHE BOOL "")
add_subdirectory(dependencies/src/glslang)

if (NOT EMSCRIPTEN)
    # volk is a Vulkan loader; there is no Vulkan ICD in a browser.
    if (WIN32)
        set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_WIN32_KHR)
    elseif(LINUX)
        set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_XLIB_KHR)
    endif()
    # macOS: no explicit platform define needed; MoltenVK handles surface extension
    add_subdirectory(dependencies/src/volk)
endif()

set(SPIRV_CROSS_CLI             OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS    OFF CACHE BOOL "")
set(SPIRV_CROSS_ENABLE_HLSL     OFF CACHE BOOL "")
set(SPIRV_CROSS_ENABLE_MSL       ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP       ON CACHE BOOL "")
set(SPIRV_CROSS_ENABLE_REFLECT   ON CACHE BOOL "")
set(SPIRV_CROSS_ENABLE_C_API    OFF CACHE BOOL "")
add_subdirectory(dependencies/src/spirv-cross)
add_subdirectory(dependencies/src/spdlog)
if (NOT EMSCRIPTEN)
    # backward-cpp's stack-trace backends (libdw/libbfd/execinfo) have no
    # Emscripten/WASM equivalent.
    # backward-cpp v1.6 uses cmake_minimum_required(VERSION 2.8) which CMake >= 3.31
    # rejects as an error. Patch the file at configure time before processing the subdirectory.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/src/backward-cpp/CMakeLists.txt" _backward_content)
    string(REGEX REPLACE
        "cmake_minimum_required\\(VERSION [0-9.]+\\)"
        "cmake_minimum_required(VERSION 3.5)"
        _backward_content "${_backward_content}")
    file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/src/backward-cpp/CMakeLists.txt" "${_backward_content}")
    unset(_backward_content)
    add_subdirectory(dependencies/src/backward-cpp)
endif()

if (VKM_ENABLE_IMGUI)
    include(dependencies/imgui.cmake)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
if (NOT EMSCRIPTEN)
    set_property(TARGET glfw PROPERTY FOLDER "ThirdPartyLibraries")
    set_property(TARGET volk PROPERTY FOLDER "ThirdPartyLibraries")
    set_property(TARGET mimalloc-static PROPERTY FOLDER "ThirdPartyLibraries")
endif()
set_property(TARGET meshoptimizer PROPERTY FOLDER "ThirdPartyLibraries")
foreach(_vkm_glslang_target glslang OSDependent SPIRV SPVRemapper)
    if (TARGET ${_vkm_glslang_target})
        set_property(TARGET ${_vkm_glslang_target} PROPERTY FOLDER "ThirdPartyLibraries/glslang")
    endif()
endforeach()
unset(_vkm_glslang_target)

# ---------------------------------------------------------------------------
# vkm-compiler tool + its dxc / Tint dependencies
#
# vkm-compiler is a native host tool; it is never built under the Emscripten
# toolchain (there is no superbuild here to produce a native binary as a side
# effect of an Emscripten configure).
# ---------------------------------------------------------------------------
if (NOT EMSCRIPTEN)
    # Locate (Windows/Linux: prebuilt archive) or build (Apple: from source)
    # the dxc HLSL->SPIR-V compiler that vkm-compiler shells out to.
    if (APPLE)
        # DXC's CMake is a full LLVM fork with its own minimum-CMake-version and
        # global CACHE pollution, and a 30-60+ minute build — nest it via
        # ExternalProject (its own CMake invocation) rather than add_subdirectory.
        include(ExternalProject)
        set(VKM_DXC_SOURCE_DIR "${PROJECT_SOURCE_DIR}/dependencies/src/dxc-macos-src")
        set(VKM_DXC_BUILD_DIR "${CMAKE_BINARY_DIR}/dxc-build")
        set(VKM_DXC_EXECUTABLE "${VKM_DXC_BUILD_DIR}/bin/dxc")
        ExternalProject_Add(dxc_build
            SOURCE_DIR "${VKM_DXC_SOURCE_DIR}"
            BINARY_DIR "${VKM_DXC_BUILD_DIR}"
            # DXC's SPIRV-Tools dependency attaches one custom command to two
            # targets, which Xcode's "new build system" (the only one left as
            # of Xcode 26) rejects. Pin this nested build to Makefiles so it
            # still works when the outer project is configured with -G Xcode.
            CMAKE_GENERATOR "Unix Makefiles"
            CMAKE_ARGS
                -C "${VKM_DXC_SOURCE_DIR}/cmake/caches/PredefinedParams.cmake"
                -DCMAKE_BUILD_TYPE=Release
                # Force clang-format "not found" so DXC's generated-header
                # verification step is skipped: the Xcode toolchain's
                # clang-format formats differently than this vendored fork
                # expects, which otherwise fails the build spuriously.
                -DCLANG_FORMAT_EXE=CLANG_FORMAT_EXE-NOTFOUND
                # DXC's vendored LLVM headers specialize std::is_nothrow_-
                # constructible (llvm/ADT/StringRef.h), which the macOS 26 CI
                # runner's AppleClang rejects as a default-error
                # (-Winvalid-specialization). Third-party code we don't patch;
                # disable the diagnostic for this nested build only.
                # -Wno-unknown-warning-option keeps older AppleClangs (which
                # don't have this diagnostic yet) from warning about it.
                "-DCMAKE_CXX_FLAGS=-Wno-unknown-warning-option -Wno-invalid-specialization"
            BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target dxc
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS "${VKM_DXC_EXECUTABLE}")
    elseif (WIN32)
        # The Windows release archive nests the exe under an arch subdirectory
        # (bin/x64/dxc.exe), unlike the flat bin/dxc of the Linux archive.
        set(VKM_DXC_EXECUTABLE "${PROJECT_SOURCE_DIR}/dependencies/src/dxc-win/bin/x64/dxc.exe")
    elseif (LINUX)
        set(VKM_DXC_EXECUTABLE "${PROJECT_SOURCE_DIR}/dependencies/src/dxc-linux/bin/dxc")
    endif()

    # Independent of VKM_USE_WEBGPU_API (which only ever turns ON under the
    # Emscripten toolchain): this lets a NATIVE vkm-compiler build exercise the
    # WGSL path directly. Requires CMake >= 3.22 (Dawn's own minimum) on the
    # host, since the nested ExternalProject configure uses the host cmake.
    option(VKM_COMPILER_ENABLE_WGSL "Build Tint support into vkm-compiler for WGSL output" OFF)
    if (VKM_COMPILER_ENABLE_WGSL)
        include(ExternalProject)
        set(VKM_DAWN_SOURCE_DIR "${PROJECT_SOURCE_DIR}/dependencies/src/dawn")
        set(VKM_DAWN_BUILD_DIR "${CMAKE_BINARY_DIR}/dawn-build")
        # tint_cmd's output lands at the Dawn build root, not under bin/.
        set(VKM_TINT_EXECUTABLE "${VKM_DAWN_BUILD_DIR}/tint")
        ExternalProject_Add(dawn_tint_build
            SOURCE_DIR "${VKM_DAWN_SOURCE_DIR}"
            BINARY_DIR "${VKM_DAWN_BUILD_DIR}"
            CMAKE_ARGS
                -DCMAKE_BUILD_TYPE=Release
                -DTINT_BUILD_SPV_READER=ON
                -DTINT_BUILD_WGSL_WRITER=ON
                -DDAWN_BUILD_SAMPLES=OFF
                -DDAWN_BUILD_TESTS=OFF
            BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target tint_cmd_tint_cmd
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS "${VKM_TINT_EXECUTABLE}")
    endif()
endif()

# Project modules
add_subdirectory(src/vkm)
add_subdirectory(tests)

# vkm-compiler links vkmcore, so it must be added after src/vkm above —
# target_link_libraries() only resolves a plain name as a CMake target if
# that target already exists at the time of the call.
if (NOT EMSCRIPTEN)
    add_subdirectory(src/tools/vkm-compiler)
endif()

option(BUILD_SAMPLES "Build samples" ON)
if (BUILD_SAMPLES)
    add_subdirectory(src/samples/empty_screen)
    add_subdirectory(src/samples/triangle)
endif()