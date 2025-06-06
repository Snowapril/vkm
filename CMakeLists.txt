# CMake version
cmake_minimum_required(VERSION 3.18.6 FATAL_ERROR)
cmake_policy(SET CMP0063 NEW) # visibility
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED 20)

if (APPLE AND NOT IOS)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.5" CACHE STRING "Minimum OS X deployment version")
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

if (UNIX AND NOT APPLE AND NOT ANDROID AND NOT WEBGL)
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

if (VKM_USE_VULKAN_API)
    add_definitions(-DVKM_USE_VULKAN_API)
endif()

if (VKM_USE_METAL_API)
    add_definitions(-DVKM_USE_METAL_API)
endif()

if (VKM_PLATFORM_WINDOWS)
    add_definitions(-DVKM_PLATFORM_WINDOWS)
endif()

if (VKM_PLATFORM_APPLE)
    add_definitions(-DVKM_PLATFORM_APPLE)
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

set(VMA_BUILD_SAMPLE                    OFF CACHE BOOL "")
set(VMA_BUILD_SAMPLE_SHADERS            OFF CACHE BOOL "")
set(VMA_STATIC_VULKAN_FUNCTIONS         OFF CACHE BOOL "")
set(VMA_DYNAMIC_VULKAN_FUNCTIONS         ON CACHE BOOL "")
set(VMA_DEBUG_INITIALIZE_ALLOCATIONS     ON CACHE BOOL "")
set(VMA_RECORDING_ENABLED                ON CACHE BOOL "")
add_subdirectory(dependencies/src/vma)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "")
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "")
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "")
set(GLFW_INSTALL        OFF CACHE BOOL "")
add_subdirectory(dependencies/src/glfw)

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

if (WIN32)
   set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_WIN32_KHR)
elseif()
   # TODO
endif()
add_subdirectory(dependencies/src/volk)

set(SPIRV_CROSS_CLI             OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS    OFF CACHE BOOL "")
set(SPIRV_CROSS_ENABLE_HLSL     OFF CACHE BOOL "")
set(SPIRV_CROSS_ENABLE_MSL      OFF CACHE BOOL "")
set(SPIRV_CROSS_ENABLE_CPP       ON CACHE BOOL "")
set(SPIRV_CROSS_ENABLE_REFLECT   ON CACHE BOOL "")
set(SPIRV_CROSS_ENABLE_C_API    OFF CACHE BOOL "")
add_subdirectory(dependencies/src/spirv-cross)
add_subdirectory(dependencies/src/spdlog)
add_subdirectory(dependencies/src/backward-cpp)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(TARGET glfw          PROPERTY FOLDER "ThirdPartyLibraries")
set_property(TARGET meshoptimizer PROPERTY FOLDER "ThirdPartyLibraries")
set_property(TARGET volk          PROPERTY FOLDER "ThirdPartyLibraries")
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