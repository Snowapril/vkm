# Target name
set(target vkmcore)
set(ROOT_DIR ${PROJECT_SOURCE_DIR})
set(LIBRARY_DIR ${PROJECT_SOURCE_DIR}/dependencies/src)
set(PUBLIC_HDR_DIR ${PROJECT_SOURCE_DIR}/include)
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../)

# Set public headers
set(PUBLIC_HDRS
    $(PUBLIC_HDR_DIR)/vkm/platform/common/app_delegate.h
    $(PUBLIC_HDR_DIR)/vkm/platform/common/window.h
    ${PUBLIC_HDR_DIR}/vkm/platform/common/input_codes.h
    ${PUBLIC_HDR_DIR}/vkm/platform/common/input_handler.h
    ${PUBLIC_HDR_DIR}/vkm/platform/common/glfw_input.h
    ${PUBLIC_HDR_DIR}/vkm/platform/common/process_stats.h
    ${PUBLIC_HDR_DIR}/vkm/base/platform.h
    ${PUBLIC_HDR_DIR}/vkm/base/common.h
    ${PUBLIC_HDR_DIR}/vkm/base/logger.h
    ${PUBLIC_HDR_DIR}/vkm/base/memory.h
    ${PUBLIC_HDR_DIR}/vkm/base/global_variable.h
    ${PUBLIC_HDR_DIR}/vkm/base/cpu_profiler.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/renderer_common.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/render_graph.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/render_graph_capture.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/render_pass.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/enum_string_util.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/pipeline_state.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/pipeline_state_parser.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/shader_cache.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/shader_cache_util.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/shader_cache_loader.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/pipeline_state_object.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/pipeline_state_manager.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/command_buffer.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/command_queue.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/driver.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/gpu_crash_handler.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/gpu_profiler.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/render_resource_pool.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/render_resource.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/texture.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/buffer.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/resource_table.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/staging_buffer.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/sampler.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/texture_view.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/buffer_view.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/gpu_offset_allocator.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/deferred_resource_reclaimer.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/swapchain.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/frame_constants.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/memory_report.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/scene/vertex_layout.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/scene/scene_model.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/scene/gltf_importer.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/scene/image_loader.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/camera.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/gbuffer.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/gi_composite.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/probe_volume.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/screenshot.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/probe_volume_updater.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/scene/scene_geometry_pool.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/scene/scene.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/scene/scene_material_tables.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/engine.h
)

# Set Sources
set(SRCS
    ${SRC_DIR}/vkm/base/common.cpp
    ${SRC_DIR}/vkm/base/logger.cpp
    ${SRC_DIR}/vkm/base/memory.cpp
    ${SRC_DIR}/vkm/base/subprocess.cpp
    ${SRC_DIR}/vkm/base/global_variable.cpp
    ${SRC_DIR}/vkm/base/cpu_profiler.cpp
    ${SRC_DIR}/vkm/platform/common/input_handler.cpp
    ${SRC_DIR}/vkm/platform/common/glfw_input.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/renderer_common.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/render_graph.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/render_graph_capture.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/pipeline_state_parser.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/shader_cache_util.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/shader_cache_loader.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/pipeline_state_object.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/pipeline_state_manager.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/bindless_resource_manager.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/command_buffer.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/command_queue.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/driver.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/gpu_crash_handler.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/gpu_profiler.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/render_resource_pool.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/render_resource.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/texture.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/buffer.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/resource_table.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/staging_buffer.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/sampler.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/texture_view.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/buffer_view.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/gpu_offset_allocator.cpp
    ${LIBRARY_DIR}/offsetAllocator/offsetAllocator.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/deferred_resource_reclaimer.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/swapchain.cpp
    ${SRC_DIR}/vkm/renderer/memory_report.cpp
    ${SRC_DIR}/vkm/renderer/scene/vertex_layout.cpp
    ${SRC_DIR}/vkm/renderer/scene/scene_model.cpp
    ${SRC_DIR}/vkm/renderer/scene/cgltf_impl.cpp
    ${SRC_DIR}/vkm/renderer/scene/gltf_importer.cpp
    ${SRC_DIR}/vkm/renderer/scene/image_loader.cpp
    ${SRC_DIR}/vkm/renderer/camera.cpp
    ${SRC_DIR}/vkm/renderer/gbuffer.cpp
    ${SRC_DIR}/vkm/renderer/gi_composite.cpp
    ${SRC_DIR}/vkm/renderer/probe_volume.cpp
    ${SRC_DIR}/vkm/renderer/probe_volume_updater.cpp
    ${SRC_DIR}/vkm/renderer/scene/stb_image_impl.cpp
    ${SRC_DIR}/vkm/renderer/scene/stb_image_write_impl.cpp
    ${SRC_DIR}/vkm/renderer/screenshot.cpp
    ${SRC_DIR}/vkm/renderer/scene/scene_geometry_pool.cpp
    ${SRC_DIR}/vkm/renderer/scene/scene.cpp
    ${SRC_DIR}/vkm/renderer/scene/scene_material_tables.cpp
    ${SRC_DIR}/vkm/renderer/engine.cpp
)

if (VKM_ENABLE_IMGUI)
    list(APPEND PUBLIC_HDRS
        ${PUBLIC_HDR_DIR}/vkm/renderer/imgui/imgui_renderer.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/imgui/render_graph_inspector.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/imgui/texture_browser.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/imgui/memory_inspector.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/imgui/cpu_profiler_inspector.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/imgui/gpu_profiler_inspector.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/renderer/imgui/imgui_renderer.cpp
        ${SRC_DIR}/vkm/renderer/imgui/render_graph_inspector.cpp
        ${SRC_DIR}/vkm/renderer/imgui/texture_browser.cpp
        ${SRC_DIR}/vkm/renderer/imgui/memory_inspector.cpp
        ${SRC_DIR}/vkm/renderer/imgui/profiler_chart_common.h
        ${SRC_DIR}/vkm/renderer/imgui/cpu_profiler_inspector.cpp
        ${SRC_DIR}/vkm/renderer/imgui/gpu_profiler_inspector.cpp
    )
endif()

if (VKM_PLATFORM_WINDOWS)
    list(APPEND PUBLIC_HDRS
        $(PUBLIC_HDR_DIR)/vkm/platform/windows/application.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/platform/windows/application.cpp
        ${SRC_DIR}/vkm/platform/windows/process_stats.cpp
    )
endif()

if (VKM_PLATFORM_LINUX)
    list(APPEND PUBLIC_HDRS
        $(PUBLIC_HDR_DIR)/vkm/platform/linux/application.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/platform/linux/application.cpp
        ${SRC_DIR}/vkm/platform/linux/process_stats.cpp
    )
endif()

if (VKM_PLATFORM_APPLE)
    list(APPEND PUBLIC_HDRS
        $(PUBLIC_HDR_DIR)/vkm/platform/apple/application.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/platform/apple/application.mm
        ${SRC_DIR}/vkm/platform/apple/process_stats.cpp
    )
endif()

if (VKM_USE_VULKAN_API)
    list(APPEND PUBLIC_HDRS
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_driver.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_swapchain.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_texture.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_buffer.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_resource_table.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_staging_buffer.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_sampler.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_texture_view.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_buffer_view.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_gpu_buffer_pool.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_util.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_command_queue.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_command_buffer.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_pipeline_state.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_bindless_resource_manager.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_frame_constant_manager.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_driver.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_swapchain.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_texture.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_buffer.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_resource_table.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_staging_buffer.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_sampler.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_texture_view.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_buffer_view.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_gpu_buffer_pool.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_util.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_command_queue.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_command_buffer.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_pipeline_state.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_bindless_resource_manager.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_frame_constant_manager.cpp
    )
    if (VKM_ENABLE_IMGUI)
        list(APPEND PUBLIC_HDRS
            ${PUBLIC_HDR_DIR}/vkm/renderer/imgui/vulkan_imgui_renderer.h
        )
        list(APPEND SRCS
            ${SRC_DIR}/vkm/renderer/imgui/vulkan_imgui_renderer.cpp
        )
    endif()
endif()

if (VKM_USE_METAL_API)
    list(APPEND PUBLIC_HDRS
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_driver.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_render_resource_pool.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_command_buffer.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_command_queue.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_swapchain.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_texture.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_pipeline_state.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_util.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_buffer.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_resource_table.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_staging_buffer.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_sampler.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_texture_view.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_buffer_view.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_gpu_heap_pool.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_bindless_resource_manager.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_frame_constant_manager.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_driver.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_render_resource_pool.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_command_buffer.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_command_queue.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_swapchain.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_texture.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_pipeline_state.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_util.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_buffer.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_resource_table.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_staging_buffer.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_sampler.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_texture_view.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_buffer_view.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_gpu_heap_pool.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_bindless_resource_manager.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_frame_constant_manager.mm
    )
    if (VKM_ENABLE_IMGUI)
        list(APPEND PUBLIC_HDRS
            ${PUBLIC_HDR_DIR}/vkm/renderer/imgui/metal_imgui_renderer.h
        )
        list(APPEND SRCS
            ${SRC_DIR}/vkm/renderer/imgui/metal_imgui_renderer.mm
        )

        # Precompile the ImGui MSL to a metallib and embed it as a byte array, so the
        # runtime loads the shader library via -[MTLDevice newLibraryWithData:]. A
        # source library compiled through MTL4Compiler does not serialize into Xcode GPU
        # captures, which made .gputrace replay crash in newLibraryWithDescriptor:.
        # -frecord-sources (gated on VKM_METAL_EMIT_MSL_SOURCE) embeds the MSL so the
        # shader debugger shows source inside a capture, matching the vfcache pipeline.
        find_program(VKM_XCRUN_EXECUTABLE xcrun)
        if (NOT VKM_XCRUN_EXECUTABLE)
            message(FATAL_ERROR "xcrun not found; required to precompile the ImGui Metal shader")
        endif()
        set(_imgui_metal_src ${SRC_DIR}/vkm/renderer/imgui/metal_imgui_shader.metal)
        set(_imgui_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
        set(_imgui_metallib ${_imgui_gen_dir}/metal_imgui_shader.metallib)
        set(_imgui_embed_hdr ${_imgui_gen_dir}/metal_imgui_shader_metallib.h)
        set(_imgui_metal_flags "")
        if (VKM_METAL_EMIT_MSL_SOURCE)
            set(_imgui_metal_flags -frecord-sources -gline-tables-only)
        endif()
        add_custom_command(
            OUTPUT ${_imgui_metallib}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_imgui_gen_dir}
            COMMAND ${VKM_XCRUN_EXECUTABLE} -sdk macosx metal ${_imgui_metal_flags}
                    -o ${_imgui_metallib} ${_imgui_metal_src}
            DEPENDS ${_imgui_metal_src}
            COMMENT "metal: compiling ImGui shader -> metallib"
            VERBATIM)
        add_custom_command(
            OUTPUT ${_imgui_embed_hdr}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${_imgui_metallib} -DOUTPUT=${_imgui_embed_hdr}
                    -DSYMBOL=kVkmImGuiShaderMetallib
                    -P ${PROJECT_SOURCE_DIR}/buildscripts/EmbedBinary.cmake
            DEPENDS ${_imgui_metallib} ${PROJECT_SOURCE_DIR}/buildscripts/EmbedBinary.cmake
            COMMENT "embedding ImGui metallib -> header"
            VERBATIM)
        # Make the .mm depend on the generated header so it is produced first and
        # triggers a recompile when the shader changes.
        set_source_files_properties(
            ${SRC_DIR}/vkm/renderer/imgui/metal_imgui_renderer.mm
            PROPERTIES OBJECT_DEPENDS ${_imgui_embed_hdr})
        # OBJECT_DEPENDS alone does not reliably re-run the metallib/header custom commands on an
        # incremental `--target triangle` build (the chain is only checked when vkmcore's own
        # objects are rebuilt). Bind the header to a custom target that vkmcore depends on so the
        # .metal -> metallib -> header chain is always checked whenever vkmcore is built.
        add_custom_target(vkm_imgui_metal_shader DEPENDS ${_imgui_embed_hdr})
        set(VKM_IMGUI_METAL_GEN_DIR ${_imgui_gen_dir})
    endif()
endif()

if (VKM_USE_WEBGPU_API)
    list(APPEND PUBLIC_HDRS
        ${PUBLIC_HDR_DIR}/vkm/platform/wasm/application.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_driver.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_swapchain.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_texture.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_buffer.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_resource_table.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_staging_buffer.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_sampler.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_texture_view.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_buffer_view.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_util.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_command_queue.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_command_buffer.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_pipeline_state.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_bindless_resource_manager.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_frame_constant_manager.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/platform/wasm/application.cpp
        ${SRC_DIR}/vkm/platform/wasm/process_stats.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_driver.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_swapchain.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_texture.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_buffer.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_resource_table.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_staging_buffer.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_sampler.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_texture_view.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_buffer_view.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_util.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_command_queue.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_command_buffer.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_pipeline_state.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_bindless_resource_manager.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_frame_constant_manager.cpp
    )
    if (VKM_ENABLE_IMGUI)
        list(APPEND PUBLIC_HDRS
            ${PUBLIC_HDR_DIR}/vkm/renderer/imgui/webgpu_imgui_renderer.h
        )
        list(APPEND SRCS
            ${SRC_DIR}/vkm/renderer/imgui/webgpu_imgui_renderer.cpp
        )
    endif()
endif()

if (VKM_PLATFORM_APPLE)
    # Compile source/headers as objective-c++
    set_source_files_properties(${SRCS} PROPERTIES
        COMPILE_FLAGS "-x objective-c++")
endif()

# offsetAllocator.cpp is vendored third-party code (never modified in-place); its own
# "conditional expression is constant" pattern trips MSVC's C4127 under /WX. Disable
# warnings-as-errors for just this one file rather than holding vendored code to our
# own strict warning bar.
if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    set(VKM_VENDORED_NO_WARN_AS_ERROR "/WX-")
else()
    set(VKM_VENDORED_NO_WARN_AS_ERROR "-Wno-error")
endif()
set_source_files_properties(${LIBRARY_DIR}/offsetAllocator/offsetAllocator.cpp PROPERTIES
    COMPILE_OPTIONS "${VKM_VENDORED_NO_WARN_AS_ERROR}")

# Build library
add_library(${target} ${SRCS} ${BACKWARD_ENABLE})

# Ensure the ImGui metallib/header are regenerated (from metal_imgui_shader.metal) before vkmcore
# builds, on incremental builds too -- see the vkm_imgui_metal_shader target above.
if (TARGET vkm_imgui_metal_shader)
    add_dependencies(${target} vkm_imgui_metal_shader)
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
    ${LIBRARY_DIR}/volk
    PRIVATE
    ${PUBLIC_HDR_DIR}
    ${LIBRARY_DIR}/glfw/include
    ${LIBRARY_DIR}/vma/src
    ${LIBRARY_DIR}/cxxopts/include
    ${LIBRARY_DIR}
    ${VKM_IMGUI_METAL_GEN_DIR} # empty unless (Metal + ImGui); holds the generated ImGui metallib header
)

# Compile options
target_compile_options(${target}
    PUBLIC
    PRIVATE
    ${DEFAULT_COMPILE_OPTIONS}
)

# Link libraries
# No shader-compilation libraries here on purpose: vkmcore only ever consumes
# already-compiled .vfcache blobs. glslang/spirv-cross are build-time concerns of
# the vkm-compiler host tool, which links them itself.
target_link_libraries(${target}
    PUBLIC
    ${DEFAULT_LINKER_OPTIONS}
	${DEFAULT_LIBRARIES}
    PRIVATE
    meshoptimizer
)

if (VKM_ENABLE_IMGUI)
    target_link_libraries(${target} PUBLIC imgui)
endif()

if (NOT EMSCRIPTEN)
    target_link_libraries(${target} PRIVATE glfw)
endif()

if (NOT EMSCRIPTEN)
    # mimalloc-static exports its own include/ dir via a PUBLIC target_include_directories,
    # so linking it is sufficient - no manual include path needed.
    target_link_libraries(${target} PRIVATE mimalloc-static)
endif()

if (VKM_USE_VULKAN_API)
    target_link_libraries(${target} PRIVATE volk VulkanMemoryAllocator)
endif()

if (EMSCRIPTEN AND VKM_USE_WEBGPU_API)
    target_compile_options(${target} PRIVATE ${VKM_EMSCRIPTEN_COMPILE_FLAGS})
    target_link_options(${target} PRIVATE ${VKM_EMSCRIPTEN_PORT_FLAGS})
endif()

if (VKM_PLATFORM_APPLE)
    if (VKM_USE_METAL_API)
        # platform/apple/application.mm references CAMetalLayer/MTLDevice directly for the
        # Metal windowing surface.
        target_link_libraries(${target} PUBLIC "-framework Metal -framework CoreVideo -framework QuartzCore")
    endif()
    if (NOT IOS)
        # Needed for the Metal branch's NSApplication/NSWindow code, and for GLFW's Cocoa
        # backend used by the Vulkan branch.
        target_link_libraries(${target} PUBLIC "-framework Cocoa -framework AppKit")
    endif()
endif()

# Compile definitions
if (EMSCRIPTEN)
    # Inside the WASM sandbox the on-disk resources path does not exist. vkmcore's only use of
    # RESOURCES_DIR is the engine PSO/shader-cache directory VkmEngine loads at startup, so it
    # points at the MEMFS mount every wasm executable preloads those two subdirectories into
    # (see VKM_WASM_ENGINE_RESOURCE_PRELOAD below).
    set(VKM_CORE_RESOURCES_DIR "/resources/")
else()
    set(VKM_CORE_RESOURCES_DIR "${RESOURCES_DIR}")
endif()

target_compile_definitions(${target}
    PRIVATE
    RESOURCES_DIR="${VKM_CORE_RESOURCES_DIR}"
    VKM_SHADER_INCLUDE_DIR="${PROJECT_SOURCE_DIR}/include/vkm/shaders"
    ${DEFAULT_COMPILE_DEFINITIONS}
)

# Runtime PSO reload recompiles shaders by invoking the same vkm-compiler the build uses
# (see VkmPipelineStateManager::reloadSource). Bake its path in only when this build
# actually has one -- an Emscripten or installed build has none, and the reload UI then
# falls back to json-only reloads rather than failing opaquely. The two-way selection
# mirrors ShaderCompile.cmake's. The in-tree vkm-compiler target is added *after* this
# directory (it links vkmcore), so that branch is applied from the top-level CMakeLists.
if (VKM_HOST_VKM_COMPILER)
    target_compile_definitions(${target} PRIVATE VKM_COMPILER_EXECUTABLE="${VKM_HOST_VKM_COMPILER}")
endif()
if (VKM_METAL_EMIT_MSL_SOURCE)
    target_compile_definitions(${target} PRIVATE VKM_COMPILER_EMIT_MSL=1)
endif()

# The --preload-file arguments a wasm executable needs so VkmEngine finds the engine PSO json and
# its compiled shader cache. Deliberately just these two subdirectories rather than all of
# resources/: resources/Scenes is fetched separately and runs to tens of megabytes.
#
# The cache directory has to exist at link time or emscripten's file_packager errors out, and under
# the Emscripten toolchain vkm_add_shader_cache_target produces nothing unless a native
# VKM_HOST_VKM_COMPILER was supplied -- so create it up front, exactly as the samples do for their
# own ShaderCache directories.
file(MAKE_DIRECTORY "${RESOURCES_DIR}Shaders/ShaderCache")
set(VKM_WASM_ENGINE_RESOURCE_PRELOAD
    "SHELL:--preload-file ${RESOURCES_DIR}Pipelines@/resources/Pipelines"
    "SHELL:--preload-file ${RESOURCES_DIR}Shaders/ShaderCache@/resources/Shaders/ShaderCache"
    CACHE INTERNAL "MEMFS preload arguments for the engine's own resources")

# Engine shader cache, globbing resources/Pipelines/Engine/*.json. The scene's GPU-driven
# compute passes (scene_cull / scene_emit_draws) are the first real engine PSOs; the older
# loose-GLSL engine shaders (scene_object/tonemap/etc.) still have no PSO json. Guard the
# dependency wiring on the target actually existing.
file(GLOB VKM_ENGINE_PSO_JSON_FILES CONFIGURE_DEPENDS
    "${RESOURCES_DIR}/Pipelines/Engine/*.json")
file(GLOB_RECURSE VKM_ENGINE_SHADER_SOURCES CONFIGURE_DEPENDS
    "${RESOURCES_DIR}/Shaders/*.hlsl" "${RESOURCES_DIR}/Shaders/*.hlsli")

vkm_add_shader_cache_target(
    TARGET_NAME vkm_engine_shaders
    OUTPUT_DIR "${RESOURCES_DIR}/Shaders/ShaderCache"
    # Engine PSO json and engine HLSL live in different directories, unlike the samples' where the
    # two sit side by side, so the shader-filepath resolution has to be pointed at the HLSL.
    SHADER_ROOT "${RESOURCES_DIR}/Shaders"
    PSO_JSON_FILES ${VKM_ENGINE_PSO_JSON_FILES}
    EXTRA_DEPENDS ${VKM_ENGINE_SHADER_SOURCES})
# Deliberately NOT a dependency of vkmcore: vkm-compiler links vkmcore, so making the library
# wait on its own shader cache is a dependency cycle (vkmcore -> vkm_engine_shaders ->
# vkm-compiler -> vkmcore). The cache is a runtime resource rather than a build input, so the
# executables that load it depend on it instead -- see src/samples/*/CMakeLists.txt and
# tests/CMakeLists.txt.

# Install
install(TARGETS
        ${target}
        RUNTIME  DESTINATION bin
        LIBRARY  DESTINATION lib
        ARCHIVE  DESTINATION lib
        PUBLIC_HEADER DESTINATION include
)