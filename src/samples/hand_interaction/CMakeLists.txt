set(target hand_interaction)
set(ROOT_DIR ${PROJECT_SOURCE_DIR})
set(PUBLIC_HDR_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Includes
set(PUBLIC_HDRS
)

# Sources
set(SRCS
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/cursor_hand_tracker.cpp
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

# macOS GUI apps have to be .app bundles (see buildscripts/AppBundle.cmake).
vkm_configure_macos_app_bundle(${target})

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
    # Inside the WASM sandbox the on-disk paths don't exist; the sample dir (PSO JSON) and the
    # generated shader caches are preloaded into MEMFS instead (see the --preload-file options).
    set(HAND_INTERACTION_SAMPLE_DIR "/SampleData/hand_interaction/")
    set(HAND_INTERACTION_SHADER_CACHE_DIR "/ShaderCache/hand_interaction/")
else()
    set(HAND_INTERACTION_SAMPLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/")
    set(HAND_INTERACTION_SHADER_CACHE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache/")
endif()

target_compile_definitions(${target}
    PRIVATE
    RESOURCES_DIR="${RESOURCES_DIR}"
    SAMPLE_DIR="${HAND_INTERACTION_SAMPLE_DIR}"
    SAMPLE_SHADER_CACHE_DIR="${HAND_INTERACTION_SHADER_CACHE_DIR}"
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

    # Same fixed-heap workaround as the other samples (see the triangle sample's CMakeLists.txt),
    # sized up because a 720p camera frame is held twice on the CPU -- once by the capture and
    # once by the consumer.
    target_link_options(${target} PRIVATE
        "SHELL:-s ALLOW_MEMORY_GROWTH=0"
        "SHELL:-s INITIAL_MEMORY=268435456"
    )

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache")
    target_link_options(${target} PRIVATE
        "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}@/SampleData/hand_interaction"
        "SHELL:--preload-file ${CMAKE_CURRENT_BINARY_DIR}/ShaderCache@/ShaderCache/hand_interaction"
        # VkmEngine loads its own PSO cache from RESOURCES_DIR, which is /resources/ under
        # Emscripten (see src/vkm/CMakeLists.txt).
        ${VKM_WASM_ENGINE_RESOURCE_PRELOAD}
    )
endif()

# Compile this sample's HLSL through vkm-compiler at build time.
vkm_add_shader_cache_target(
    TARGET_NAME hand_interaction_shaders
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache"
    PSO_JSON_FILES "${CMAKE_CURRENT_SOURCE_DIR}/hand_background.json"
                   "${CMAKE_CURRENT_SOURCE_DIR}/hand_sphere.json"
    EXTRA_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hand_background.hlsl"
                  "${CMAKE_CURRENT_SOURCE_DIR}/hand_sphere.hlsl")
if (TARGET vkm_engine_shaders)
    # Not for this sample's own PSOs -- it owns both of those -- but because VkmEngine loads the
    # engine PSO set at startup, and a wasm build packs that cache into its MEMFS bundle at link
    # time. Without this the bundle is packed from whatever the cache directory happened to hold.
    add_dependencies(${target} vkm_engine_shaders)
endif()
if (TARGET hand_interaction_shaders)
    add_dependencies(${target} hand_interaction_shaders)
    if (EMSCRIPTEN AND VKM_USE_WEBGPU_API)
        # Repack the .data bundle when regenerated caches change (see the triangle sample).
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_BINARY_DIR}/hand_interaction_shaders.stamps/hand_background.0.stamp")
    endif()
endif()
