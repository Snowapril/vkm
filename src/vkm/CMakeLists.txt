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
    ${PUBLIC_HDR_DIR}/vkm/base/platform.h
    ${PUBLIC_HDR_DIR}/vkm/base/common.h
    ${PUBLIC_HDR_DIR}/vkm/base/logger.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/renderer_common.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/render_graph.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/render_pass.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/enum_string_util.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/pipeline_state.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/pipeline_state_parser.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/command_buffer.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/command_queue.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/driver.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/render_resource_pool.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/render_resource.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/texture.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/swapchain.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/engine.h
)

# Set Sources
set(SRCS
    ${SRC_DIR}/vkm/base/common.cpp
    ${SRC_DIR}/vkm/base/logger.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/renderer_common.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/render_graph.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/pipeline_state_parser.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/command_buffer.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/command_queue.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/driver.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/render_resource_pool.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/render_resource.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/texture.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/swapchain.cpp
    ${SRC_DIR}/vkm/renderer/engine.cpp
)

if (VKM_ENABLE_IMGUI)
    list(APPEND PUBLIC_HDRS
        ${PUBLIC_HDR_DIR}/vkm/renderer/imgui/imgui_renderer.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/renderer/imgui/imgui_renderer.cpp
    )
endif()

if (VKM_PLATFORM_WINDOWS)
    list(APPEND PUBLIC_HDRS
        $(PUBLIC_HDR_DIR)/vkm/platform/windows/application.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/platform/windows/application.cpp
    )
endif()

if (VKM_PLATFORM_LINUX)
    list(APPEND PUBLIC_HDRS
        $(PUBLIC_HDR_DIR)/vkm/platform/linux/application.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/platform/linux/application.cpp
    )
endif()

if (VKM_PLATFORM_APPLE)
    list(APPEND PUBLIC_HDRS
        $(PUBLIC_HDR_DIR)/vkm/platform/apple/application.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/platform/apple/application.mm
    )
endif()

if (VKM_USE_VULKAN_API)
    list(APPEND PUBLIC_HDRS
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_driver.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_swapchain.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_texture.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_util.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_command_queue.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_command_buffer.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_driver.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_swapchain.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_texture.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_util.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_command_queue.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_command_buffer.cpp
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
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_command_buffer.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_command_queue.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_swapchain.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_texture.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_driver.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_command_buffer.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_command_queue.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_swapchain.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_texture.mm
    )
    if (VKM_ENABLE_IMGUI)
        list(APPEND PUBLIC_HDRS
            ${PUBLIC_HDR_DIR}/vkm/renderer/imgui/metal_imgui_renderer.h
        )
        list(APPEND SRCS
            ${SRC_DIR}/vkm/renderer/imgui/metal_imgui_renderer.mm
        )
    endif()
endif()

if (VKM_USE_WEBGPU_API)
    list(APPEND PUBLIC_HDRS
        ${PUBLIC_HDR_DIR}/vkm/platform/wasm/application.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_driver.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_swapchain.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_texture.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_util.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_command_queue.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/webgpu/webgpu_command_buffer.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/platform/wasm/application.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_driver.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_swapchain.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_texture.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_util.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_command_queue.cpp
        ${SRC_DIR}/vkm/renderer/backend/webgpu/webgpu_command_buffer.cpp
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

# Build library
add_library(${target} ${SRCS} ${BACKWARD_ENABLE})

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
    ${LIBRARY_DIR}/glslang
    ${LIBRARY_DIR}/vma/src
    ${LIBRARY_DIR}
)

# Compile options
target_compile_options(${target}
    PUBLIC
    PRIVATE
    ${DEFAULT_COMPILE_OPTIONS}
)

# Link libraries
target_link_libraries(${target}
    PUBLIC
    ${DEFAULT_LINKER_OPTIONS}
	${DEFAULT_LIBRARIES}
    PRIVATE
    glslang
    SPIRV
    spirv-cross-core
)

if (VKM_ENABLE_IMGUI)
    target_link_libraries(${target} PUBLIC imgui)
endif()

if (NOT EMSCRIPTEN)
    target_link_libraries(${target} PRIVATE glfw)
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
target_compile_definitions(${target}
    PRIVATE
    RESOURCES_DIR="${RESOURCES_DIR}"
    ${DEFAULT_COMPILE_DEFINITIONS}
)

# Install
install(TARGETS  
        ${target} 
        RUNTIME  DESTINATION bin
        LIBRARY  DESTINATION lib
        ARCHIVE  DESTINATION lib
        PUBLIC_HEADER DESTINATION include
)