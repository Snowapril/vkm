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

# Set enable output of compile commands during generation
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

# Compile options
include(buildscripts/CompileOptions.cmake)

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
    # dependencies/src is bootstrapped fresh (git-cloned) by bootstrap.py and is not
    # tracked by this repo's git history, so a plain edit to the vendored file would be
    # silently lost on the next bootstrap (including in CI). Reapply the patch here,
    # idempotently, every configure. See src/vkm/renderer/backend/webgpu/AGENTS.md and
    # buildscripts/patches/spdlog-fmt-emscripten-consteval.patch for why this exists.
    # Also required on Apple platforms: a newer AppleClang (macOS 26 SDK toolchain)
    # reproduces the same fmt consteval bug the patch was originally written for.
    set(_vkm_spdlog_dir "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/src/spdlog")
    set(_vkm_spdlog_patch "${CMAKE_CURRENT_SOURCE_DIR}/buildscripts/patches/spdlog-fmt-emscripten-consteval.patch")
    if (EXISTS "${_vkm_spdlog_dir}" AND EXISTS "${_vkm_spdlog_patch}")
        execute_process(
            COMMAND git apply --reverse --check "${_vkm_spdlog_patch}"
            WORKING_DIRECTORY "${_vkm_spdlog_dir}"
            RESULT_VARIABLE _vkm_spdlog_patch_already_applied
            OUTPUT_QUIET ERROR_QUIET
        )
        if (NOT _vkm_spdlog_patch_already_applied EQUAL 0)
            execute_process(
                COMMAND git apply "${_vkm_spdlog_patch}"
                WORKING_DIRECTORY "${_vkm_spdlog_dir}"
                RESULT_VARIABLE _vkm_spdlog_patch_result
            )
            if (NOT _vkm_spdlog_patch_result EQUAL 0)
                message(FATAL_ERROR "Failed to apply required spdlog/fmt consteval patch (buildscripts/patches/spdlog-fmt-emscripten-consteval.patch)")
            endif()
        endif()
    endif()
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
option(CHROME_TRACING "Enable Chrome Tracing for CPU profiling" ON)
option(TASKFLOW_PROFILER, "Enable Taskflow Profiler" ON)

if (CMAKE_BUILD_TYPE MATCHES Debug AND CMAKE_COMPILER_IS_GNUCXX AND BUILD_COVERAGE)
    include(CodeCoverage)
    setup_target_for_coverage(${PROJECT_NAME}_coverage UnitTests coverage)
endif()

if (GPU_RESOURCE_DEBUG_NAME)
    add_definitions(-DVKM_DEBUG_NAME_ENABLED)
endif()

if (CHROME_TRACING)
    add_definitions(-DENABLE_CHROME_TRACING)
endif()

if (TASKFLOW_PROFILER)
    # You can visualize profiled json file at https://taskflow.github.io/tfprof/ 
    add_definitions(-DTF_ENABLE_PROFILER=taskflow_profile.json)
endif()

# Overrides
set(CMAKE_MACOSX_RPATH ON)

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
set(SPIRV_CROSS_ENABLE_MSL      OFF CACHE BOOL "")
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

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
if (NOT EMSCRIPTEN)
    set_property(TARGET glfw PROPERTY FOLDER "ThirdPartyLibraries")
    set_property(TARGET volk PROPERTY FOLDER "ThirdPartyLibraries")
endif()
set_property(TARGET meshoptimizer PROPERTY FOLDER "ThirdPartyLibraries")
set_property(TARGET glslang       PROPERTY FOLDER "ThirdPartyLibraries/glslang")
set_property(TARGET OSDependent   PROPERTY FOLDER "ThirdPartyLibraries/glslang")
set_property(TARGET SPIRV         PROPERTY FOLDER "ThirdPartyLibraries/glslang")
set_property(TARGET SPVRemapper   PROPERTY FOLDER "ThirdPartyLibraries/glslang")

# Project modules
add_subdirectory(src/vkm)
add_subdirectory(tests)

option(BUILD_SAMPLES "Build samples" ON)
if (BUILD_SAMPLES)
    add_subdirectory(src/samples/triangle)
endif()