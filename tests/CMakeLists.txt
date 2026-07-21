set(target UnitTests)
set(ROOT_DIR ${PROJECT_SOURCE_DIR})
set(PUBLIC_HDR_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Includes
set(PUBLIC_HDRS 
    ${PUBLIC_HDR_DIR}/UnitTestUnits.hpp
)

# Sources
set(SRCS
    ${SRC_DIR}/UnitTests.cpp
    ${SRC_DIR}/TestEngineSetup.cpp
    ${SRC_DIR}/TestPsoConfig.cpp
    ${SRC_DIR}/TestMemoryTracker.cpp
    ${SRC_DIR}/TestShaderCacheUtil.cpp
    ${SRC_DIR}/TestPipelineStateManager.cpp
    ${SRC_DIR}/TestBindlessSlotAllocator.cpp
    ${SRC_DIR}/TestInputHandler.cpp
    ${SRC_DIR}/TestClipSpaceOrientation.cpp
    ${SRC_DIR}/TestGlobalVariable.cpp
)

if (VKM_USE_METAL_API)
    list(APPEND SRCS
        ${SRC_DIR}/TestMetalDriver.mm
        ${SRC_DIR}/TestBackbufferReadback.mm
        ${SRC_DIR}/TestMetalBindlessTriangle.mm
        ${SRC_DIR}/TestRenderGraphCapture.mm
        ${SRC_DIR}/TestClipSpaceOrientationMetal.mm
    )
endif()

# Build executable
add_executable(${target} ${SRCS} ${BACKWARD_ENABLE})

if (VKM_USE_METAL_API)
    set_source_files_properties(
        ${SRC_DIR}/TestMetalDriver.mm
        ${SRC_DIR}/TestBackbufferReadback.mm
        ${SRC_DIR}/TestMetalBindlessTriangle.mm
        ${SRC_DIR}/TestRenderGraphCapture.mm
        ${SRC_DIR}/TestClipSpaceOrientationMetal.mm
        PROPERTIES COMPILE_FLAGS "-x objective-c++"
    )
endif()

# TestMetalBindlessTriangle and TestClipSpaceOrientation* drive the real triangle sample
# PSO; compile its shader caches for this build's backend next to the test binary. Needed
# by both the Metal and Vulkan backends.
if (VKM_USE_METAL_API OR VKM_USE_VULKAN_API)
    vkm_add_shader_cache_target(
        TARGET_NAME   tests_triangle_shaders
        OUTPUT_DIR    "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache"
        PSO_JSON_FILES "${ROOT_DIR}/src/samples/triangle/renderpass.json"
        EXTRA_DEPENDS  "${ROOT_DIR}/src/samples/triangle/triangle.hlsl")
endif()

if (NOT EMSCRIPTEN)
    # backward-cpp has no Emscripten/WASM stack-trace backend.
    add_backward(${target})
endif()

# Project options
set_target_properties(${target}
    PROPERTIES
    ${DEFAULT_PROJECT_OPTIONS}
    PUBLIC_HEADER "${PUBLIC_HDRS}"
)

#Include directories
target_include_directories(${target}
    PUBLIC
    $<BUILD_INTERFACE:${PUBLIC_HDR_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_PREFIX}/include>
    PRIVATE
    ${ROOT_DIR}/dependencies/src/doctest
    ${ROOT_DIR}/dependencies/src/taskflow
    ${ROOT_DIR}/dependencies/src/stb
    ${ROOT_DIR}/include
)

# Compile options
target_compile_options(${target}
    PRIVATE
    ${DEFAULT_COMPILE_OPTIONS}
)

# Compile definitions
if (EMSCRIPTEN AND VKM_USE_WEBGPU_API)
    # resources/ is preloaded into the Emscripten test binary's MEMFS virtual filesystem
    # (see the --preload-file link option below) under /resources/, not the real on-disk
    # RESOURCES_DIR path, which doesn't exist inside the WASM sandbox.
    set(TEST_RESOURCES_DIR "/resources/")
else()
    set(TEST_RESOURCES_DIR "${RESOURCES_DIR}")
endif()

target_compile_definitions(${target}
    PRIVATE
    RESOURCES_DIR="${TEST_RESOURCES_DIR}"
    ${DEFAULT_COMPILE_DEFINITIONS}
)

if (VKM_USE_METAL_API OR VKM_USE_VULKAN_API)
    add_dependencies(${target} tests_triangle_shaders)
    target_compile_definitions(${target}
        PRIVATE
        TEST_TRIANGLE_SAMPLE_DIR="${ROOT_DIR}/src/samples/triangle/"
        TEST_TRIANGLE_SHADER_CACHE_DIR="${CMAKE_CURRENT_BINARY_DIR}/ShaderCache/"
    )
endif()

# Link libraries
target_link_libraries(${target}
    PUBLIC
    ${DEFAULT_LINKER_OPTIONS}
	${DEFAULT_LIBRARIES}
    vkmcore
)

if (EMSCRIPTEN AND VKM_USE_WEBGPU_API)
    target_compile_options(${target} PRIVATE ${VKM_EMSCRIPTEN_COMPILE_FLAGS})
    target_link_options(${target} PRIVATE ${VKM_EMSCRIPTEN_PORT_FLAGS})

    # VKM_EMSCRIPTEN_PORT_FLAGS sets ALLOW_MEMORY_GROWTH=1 project-wide, which makes the
    # WASM heap's backing ArrayBuffer "resizable" (a `maximum` is specified to
    # WebAssembly.Memory). Current Chrome/V8 rejects calling TextDecoder.decode() on a
    # resizable ArrayBuffer, which crashes real WebGPU adapter negotiation under headless
    # Chrome specifically in this test binary. UnitTests' heap usage is small and known
    # ahead of time (unlike the sample/vkmcore, which may need to grow), so disable growth
    # here and fix a generous initial size instead — with no `maximum`, the buffer is a
    # plain non-resizable ArrayBuffer, sidestepping the V8 restriction entirely. emcc's
    # `-s` settings take the last-specified value, so this must come after
    # VKM_EMSCRIPTEN_PORT_FLAGS above to win.
    target_link_options(${target} PRIVATE
        "SHELL:-s ALLOW_MEMORY_GROWTH=0"
        "SHELL:-s INITIAL_MEMORY=134217728"
    )

    # Mounts resources/ into the test binary's MEMFS virtual filesystem at /resources/ so
    # fixture-reading tests (e.g. TestPsoConfig.cpp, TestPipelineStateManager.cpp) can find
    # their JSON files under RESOURCES_DIR (see TEST_RESOURCES_DIR above). resources/ is
    # small (tens of KB), so preloading the whole directory is simpler than curating a subset.
    target_link_options(${target} PRIVATE
        "SHELL:--preload-file ${ROOT_DIR}/resources@/resources"
    )
endif()
