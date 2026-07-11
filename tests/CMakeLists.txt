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
)

if (VKM_USE_METAL_API)
    list(APPEND SRCS
        ${SRC_DIR}/TestMetalDriver.mm
        ${SRC_DIR}/TestBackbufferReadback.mm
    )
endif()

# Build executable
add_executable(${target} ${SRCS} ${BACKWARD_ENABLE})

if (VKM_USE_METAL_API)
    set_source_files_properties(
        ${SRC_DIR}/TestMetalDriver.mm
        ${SRC_DIR}/TestBackbufferReadback.mm
        PROPERTIES COMPILE_FLAGS "-x objective-c++"
    )
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
target_compile_definitions(${target}
    PRIVATE
    RESOURCES_DIR="${RESOURCES_DIR}"
    ${DEFAULT_COMPILE_DEFINITIONS}
)

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
endif()
