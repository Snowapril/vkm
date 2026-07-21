# ShaderCompile.cmake
#
# vkm_add_shader_cache_target(): wire a set of PSO JSON files through the
# vkm-compiler build-time tool, producing <name>.<stage>.<backend>.vfcache
# files in OUTPUT_DIR. Each PSO json gets an add_custom_command that reruns
# vkm-compiler whenever the json, any EXTRA_DEPENDS (HLSL sources), or the
# vkm-compiler binary itself changes; an umbrella custom target built as part
# of ALL depends on all resulting stamp files.
#
# Usage:
#   vkm_add_shader_cache_target(
#       TARGET_NAME   my_shaders
#       OUTPUT_DIR    "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache"
#       PSO_JSON_FILES "${CMAKE_CURRENT_SOURCE_DIR}/renderpass.json"
#       EXTRA_DEPENDS  "${CMAKE_CURRENT_SOURCE_DIR}/triangle.hlsl")
#
# Deliberately narrow: this repo has no other add_custom_command users yet, so
# the surface is kept specific to this single use rather than generalized.
function(vkm_add_shader_cache_target)
    cmake_parse_arguments(SC
        ""
        "TARGET_NAME;OUTPUT_DIR"
        "PSO_JSON_FILES;EXTRA_DEPENDS"
        ${ARGN})

    if (NOT SC_TARGET_NAME)
        message(FATAL_ERROR "vkm_add_shader_cache_target: TARGET_NAME is required")
    endif()
    if (NOT SC_OUTPUT_DIR)
        message(FATAL_ERROR "vkm_add_shader_cache_target: OUTPUT_DIR is required")
    endif()

    # vkm-compiler is a native host tool that cannot itself be an Emscripten
    # binary; there is no superbuild here to build it as a side effect of an
    # Emscripten-toolchain configure. A prebuilt native vkm-compiler can be
    # supplied via -DVKM_HOST_VKM_COMPILER=<path> (scripts/run_sample.py does
    # this for the webgpu backend); otherwise skip (warn, don't fail).
    if (EMSCRIPTEN AND NOT VKM_HOST_VKM_COMPILER)
        message(STATUS "vkm_add_shader_cache_target(${SC_TARGET_NAME}): skipped under Emscripten toolchain (set VKM_HOST_VKM_COMPILER to enable)")
        return()
    endif()
    if (VKM_HOST_VKM_COMPILER AND NOT EXISTS "${VKM_HOST_VKM_COMPILER}")
        message(FATAL_ERROR "vkm_add_shader_cache_target(${SC_TARGET_NAME}): VKM_HOST_VKM_COMPILER points to a non-existent file: ${VKM_HOST_VKM_COMPILER}")
    endif()

    if (NOT SC_PSO_JSON_FILES)
        message(STATUS "vkm_add_shader_cache_target(${SC_TARGET_NAME}): no PSO JSON files given; nothing to compile yet")
        return()
    endif()

    # Single-active-backend-per-build, matching the rest of the repo.
    if (VKM_USE_VULKAN_API)
        set(_backend vulkan)
    elseif (VKM_USE_METAL_API)
        set(_backend metal)
    elseif (VKM_USE_WEBGPU_API)
        set(_backend webgpu)
    else()
        message(STATUS "vkm_add_shader_cache_target(${SC_TARGET_NAME}): no active backend; skipped")
        return()
    endif()

    # Metal only: forward the MSL-source/debug-info toggle (see VKM_METAL_EMIT_MSL_SOURCE).
    set(_emit_msl_arg "")
    if (_backend STREQUAL metal AND VKM_METAL_EMIT_MSL_SOURCE)
        set(_emit_msl_arg --emit-msl)
    endif()

    # Either the in-tree vkm-compiler target or the externally supplied host binary.
    if (VKM_HOST_VKM_COMPILER)
        set(_compiler "${VKM_HOST_VKM_COMPILER}")
        set(_compiler_dep "${VKM_HOST_VKM_COMPILER}")
    else()
        set(_compiler "$<TARGET_FILE:vkm-compiler>")
        set(_compiler_dep vkm-compiler)
    endif()

    set(_stamp_dir "${CMAKE_CURRENT_BINARY_DIR}/${SC_TARGET_NAME}.stamps")
    file(MAKE_DIRECTORY "${_stamp_dir}")

    set(_stamps "")
    set(_index 0)
    foreach (_pso IN LISTS SC_PSO_JSON_FILES)
        get_filename_component(_pso_name "${_pso}" NAME_WE)
        set(_stamp "${_stamp_dir}/${_pso_name}.${_index}.stamp")
        add_custom_command(
            OUTPUT "${_stamp}"
            COMMAND ${_compiler}
                    --pso "${_pso}"
                    --output-dir "${SC_OUTPUT_DIR}"
                    --backend ${_backend}
                    ${_emit_msl_arg}
            COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
            DEPENDS "${_pso}" ${SC_EXTRA_DEPENDS} ${_compiler_dep}
            COMMENT "vkm-compiler: compiling shaders for ${_pso_name} (${_backend})"
            VERBATIM)
        list(APPEND _stamps "${_stamp}")
        math(EXPR _index "${_index} + 1")
    endforeach()

    add_custom_target(${SC_TARGET_NAME} ALL DEPENDS ${_stamps})
endfunction()
