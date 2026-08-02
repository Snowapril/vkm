set(target gi)
set(ROOT_DIR ${PROJECT_SOURCE_DIR})
set(PUBLIC_HDR_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(PUBLIC_HDRS
)

set(SRCS
    ${SRC_DIR}/main.cpp
)

if (VKM_PLATFORM_APPLE)
    set_source_files_properties(${SRCS} PROPERTIES
        COMPILE_FLAGS "-x objective-c++")
endif()

add_executable(${target} ${SRCS} ${BACKWARD_ENABLE})

if (NOT EMSCRIPTEN)
    add_backward(${target})
endif()

set_target_properties(${target}
    PROPERTIES
    ${DEFAULT_PROJECT_OPTIONS}
    PUBLIC_HEADER "${PUBLIC_HDRS}"
)

vkm_configure_macos_app_bundle(${target})

target_include_directories(${target}
    PUBLIC
    $<BUILD_INTERFACE:${PUBLIC_HDR_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_PREFIX}/include>
    PRIVATE
    ${ROOT_DIR}/dependencies/src/cxxopts/include
    ${ROOT_DIR}/include
)

target_compile_options(${target}
    PRIVATE
    ${DEFAULT_COMPILE_OPTIONS}
)

target_compile_definitions(${target}
    PRIVATE
    RESOURCES_DIR="${RESOURCES_DIR}"
    ${DEFAULT_COMPILE_DEFINITIONS}
)

target_link_libraries(${target}
    PUBLIC
    ${DEFAULT_LINKER_OPTIONS}
	${DEFAULT_LIBRARIES}
    vkmcore
)

if (EMSCRIPTEN AND VKM_USE_WEBGPU_API)
    target_compile_options(${target} PRIVATE ${VKM_EMSCRIPTEN_COMPILE_FLAGS})
    target_link_options(${target} PRIVATE ${VKM_EMSCRIPTEN_PORT_FLAGS})

    # Same fixed-heap workaround as the other samples, sized for a glTF import.
    target_link_options(${target} PRIVATE
        "SHELL:-s ALLOW_MEMORY_GROWTH=0"
        "SHELL:-s INITIAL_MEMORY=536870912"
    )

    # Unlike the other samples this one owns no shaders: every pass it runs is an engine PSO, so
    # the engine's own preload is all it needs. Scenes still have to be mounted by other means --
    # they live outside the source tree and are far too large to bake into MEMFS.
    target_link_options(${target} PRIVATE
        ${VKM_WASM_ENGINE_RESOURCE_PRELOAD}
    )
endif()

if (TARGET vkm_engine_shaders)
    # Everything: the scene's cull/emit, the G-buffer, both lighting passes, the probe passes, the
    # composite and the tone map are all engine PSOs.
    add_dependencies(${target} vkm_engine_shaders)
endif()
