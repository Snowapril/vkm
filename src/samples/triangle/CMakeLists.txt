set(target triangle)
set(ROOT_DIR ${PROJECT_SOURCE_DIR})
set(PUBLIC_HDR_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Includes
set(PUBLIC_HDRS 
)

# Sources
set(SRCS
    ${SRC_DIR}/main.cpp
)

if (VKM_PLATFORM_APPLE)
    # Compile source/headers as objective-c++
    set_source_files_properties(${SRCS} PROPERTIES
        COMPILE_FLAGS "-x objective-c++")
endif()

# Build executable
add_executable(${target} ${SRCS} ${BACKWARD_ENABLE})

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
    ${ROOT_DIR}/dependencies/src/cxxopts/include
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
    SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/"
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
endif()

# Compile this sample's HLSL through vkm-compiler at build time. This is the
# concrete end-to-end instance that proves out the whole shader pipeline.
vkm_add_shader_cache_target(
    TARGET_NAME triangle_shaders
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache"
    PSO_JSON_FILES "${CMAKE_CURRENT_SOURCE_DIR}/renderpass.json"
    EXTRA_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/triangle.hlsl")
if (TARGET triangle_shaders)
    add_dependencies(${target} triangle_shaders)
endif()
