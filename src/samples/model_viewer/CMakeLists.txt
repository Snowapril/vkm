set(target model_viewer)
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
if (EMSCRIPTEN AND VKM_USE_WEBGPU_API)
    # Inside the WASM sandbox the on-disk paths don't exist; the sample dir (renderpass
    # JSON) and the generated shader caches are preloaded into MEMFS instead (see the
    # --preload-file link options below).
    set(MODEL_VIEWER_SAMPLE_DIR "/SampleData/model_viewer/")
    set(MODEL_VIEWER_SHADER_CACHE_DIR "/ShaderCache/model_viewer/")
else()
    set(MODEL_VIEWER_SAMPLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/")
    set(MODEL_VIEWER_SHADER_CACHE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache/")
endif()

target_compile_definitions(${target}
    PRIVATE
    RESOURCES_DIR="${RESOURCES_DIR}"
    SAMPLE_DIR="${MODEL_VIEWER_SAMPLE_DIR}"
    SAMPLE_SHADER_CACHE_DIR="${MODEL_VIEWER_SHADER_CACHE_DIR}"
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

    # Same fixed-heap workaround as the triangle sample (see its CMakeLists.txt), sized up
    # because a glTF scene's vertex/index data is held CPU-side during import.
    target_link_options(${target} PRIVATE
        "SHELL:-s ALLOW_MEMORY_GROWTH=0"
        "SHELL:-s INITIAL_MEMORY=536870912"
    )

    # Only the sample dir and shader caches are preloaded -- scenes live outside the source
    # tree (resources/Scenes, fetched by scripts/download_scenes.py) and are far too large
    # to bake into MEMFS, so a wasm build needs a scene mounted by other means.
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache")
    target_link_options(${target} PRIVATE
        "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}@/SampleData/model_viewer"
        "SHELL:--preload-file ${CMAKE_CURRENT_BINARY_DIR}/ShaderCache@/ShaderCache/model_viewer"
    )
endif()

# Compile this sample's HLSL through vkm-compiler at build time.
vkm_add_shader_cache_target(
    TARGET_NAME model_viewer_shaders
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache"
    PSO_JSON_FILES "${CMAKE_CURRENT_SOURCE_DIR}/renderpass.json"
    EXTRA_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/model_viewer.hlsl")
if (TARGET model_viewer_shaders)
    add_dependencies(${target} model_viewer_shaders)
    if (EMSCRIPTEN AND VKM_USE_WEBGPU_API)
        # Repack the .data bundle when regenerated caches change (see the triangle sample).
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_BINARY_DIR}/model_viewer_shaders.stamps/renderpass.0.stamp")
    endif()
endif()
