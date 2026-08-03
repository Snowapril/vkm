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

    # Opt-in scene preload, so the WebGPU render path can be *looked at* rather than only built and
    # unit-tested -- which restir.md records as the last unverified part of the Phase 4 gate.
    # Point it at a small glTF directory (DamagedHelmet, ~4 MB); Sponza-scale assets exceed both
    # MEMFS sensibility and this backend's fixed bindless mega-buffers.
    #   -DVKM_WASM_GI_SCENE=resources/Scenes/DamagedHelmet
    if (VKM_WASM_GI_SCENE)
        get_filename_component(VKM_WASM_GI_SCENE_ABS "${VKM_WASM_GI_SCENE}" ABSOLUTE BASE_DIR "${ROOT_DIR}")
        get_filename_component(VKM_WASM_GI_SCENE_NAME "${VKM_WASM_GI_SCENE_ABS}" NAME)
        target_link_options(${target} PRIVATE
            "SHELL:--preload-file ${VKM_WASM_GI_SCENE_ABS}@/scene"
        )
        # A wasm build has no command line, so the preloaded scene has to be the default path.
        target_compile_definitions(${target} PRIVATE
            VKM_WASM_GI_SCENE_GLTF="/scene/${VKM_WASM_GI_SCENE_NAME}.gltf")
        # There is no command line either, so capturing a frame has to be baked in. Off by
        # default -- it makes the sample exit after one screenshot, which is wrong for an
        # interactive wasm build -- and turned on only for a headless verification run:
        #   -DVKM_WASM_GI_SCENE=... -DVKM_WASM_GI_AUTO_SCREENSHOT=ON
        if (VKM_WASM_GI_AUTO_SCREENSHOT)
            # Which frame to capture. Deliberately tiny: a headless run rasterizes in software
            # (SwiftShader), and this sample draws every budgeted probe's six cube faces per frame,
            # so the interactive default of 600 is unreachable -- one measured run spent 24 hours
            # and 127 CPU-hours without getting there. A handful of frames is enough to show the
            # frame renders; it is not enough for the probe volume to converge.
            if (NOT VKM_WASM_GI_SCREENSHOT_FRAME)
                set(VKM_WASM_GI_SCREENSHOT_FRAME 4)
            endif()
            target_compile_definitions(${target} PRIVATE
                VKM_WASM_GI_AUTO_SCREENSHOT=1
                VKM_WASM_GI_SCREENSHOT_FRAME=${VKM_WASM_GI_SCREENSHOT_FRAME})
        endif()
        message(STATUS "gi: preloading ${VKM_WASM_GI_SCENE_ABS} into MEMFS at /scene")
    endif()
endif()

if (TARGET vkm_engine_shaders)
    # Everything: the scene's cull/emit, the G-buffer, both lighting passes, the probe passes, the
    # composite and the tone map are all engine PSOs.
    add_dependencies(${target} vkm_engine_shaders)
endif()
