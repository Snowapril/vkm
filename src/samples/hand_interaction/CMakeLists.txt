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
    ${SRC_DIR}/hand_input_mouse.cpp
)

if (VKM_PLATFORM_APPLE)
    # Compile source/headers as objective-c++
    set_source_files_properties(${SRCS} PROPERTIES
        COMPILE_FLAGS "-x objective-c++")
    # Already .mm, so it needs no -x flag; it carries createPlatformHandInput() here.
    list(APPEND SRCS ${SRC_DIR}/hand_input_apple.mm)
else()
    list(APPEND SRCS ${SRC_DIR}/hand_input_null.cpp)
endif()

# Build executable
add_executable(${target} ${SRCS} ${BACKWARD_ENABLE})

add_backward(${target})

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
target_compile_definitions(${target}
    PRIVATE
    RESOURCES_DIR="${RESOURCES_DIR}"
    SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/"
    SAMPLE_SHADER_CACHE_DIR="${CMAKE_CURRENT_BINARY_DIR}/ShaderCache/"
    ${DEFAULT_COMPILE_DEFINITIONS}
)

# Link libraries
target_link_libraries(${target}
    PUBLIC
    ${DEFAULT_LINKER_OPTIONS}
	${DEFAULT_LIBRARIES}
    vkmcore
)

if (VKM_PLATFORM_APPLE)
    # AVFoundation captures the frames and Vision detects the hand pose in them; CoreMedia and
    # CoreVideo carry the sample and pixel buffers between the two.
    target_link_libraries(${target} PUBLIC
        "-framework AVFoundation -framework CoreMedia -framework CoreVideo -framework Vision")
endif()

# Compile this sample's HLSL through vkm-compiler at build time.
vkm_add_shader_cache_target(
    TARGET_NAME hand_interaction_shaders
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache"
    PSO_JSON_FILES "${CMAKE_CURRENT_SOURCE_DIR}/hand_background.json"
                   "${CMAKE_CURRENT_SOURCE_DIR}/hand_sphere.json"
    EXTRA_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hand_background.hlsl"
                  "${CMAKE_CURRENT_SOURCE_DIR}/hand_sphere.hlsl")
if (TARGET hand_interaction_shaders)
    add_dependencies(${target} hand_interaction_shaders)
endif()
