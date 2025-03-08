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
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/command_buffer.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/command_dispatcher.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/command_queue.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/driver.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/driver_resource_pool.h
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
    ${SRC_DIR}/vkm/renderer/backend/common/command_buffer.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/command_dispatcher.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/command_queue.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/driver.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/driver_resource_pool.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/render_resource.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/texture.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/swapchain.cpp
    ${SRC_DIR}/vkm/renderer/engine.cpp
)

if (VKM_USE_VULKAN_API)
    list(APPEND PUBLIC_HDRS
        $(PUBLIC_HDR_DIR)/vkm/platform/windows/application.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_driver.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_swapchain.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_texture.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_util.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/platform/windows/application.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_driver.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_swapchain.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_texture.cpp
        ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_util.cpp
    )
endif()

if (VKM_USE_METAL_API)
    list(APPEND PUBLIC_HDRS
        $(PUBLIC_HDR_DIR)/vkm/platform/apple/application.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_driver.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_command_buffer.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_command_queue.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_swapchain.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_texture.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/platform/apple/application.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_driver.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_command_buffer.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_command_queue.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_swapchain.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_texture.mm
    )
endif()

if (VKM_PLATFORM_APPLE)
    # Compile source/headers as objective-c++
    set_source_files_properties(${SRCS} PROPERTIES
        COMPILE_FLAGS "-x objective-c++")
endif()

# Build library
add_library(${target} ${SRCS} ${BACKWARD_ENABLE})

# Enable backward-cpp stack-trace for this build
add_backward(${target})

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
    ${PUBLIC_HDR_DIR}
    ${LIBRARY_DIR}/glfw/include
    ${LIBRARY_DIR}/volk
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
    glfw
    volk
    glslang
    SPIRV
    spirv-cross-core
    VulkanMemoryAllocator
)

if (VKM_USE_METAL_API)
    target_link_libraries(${target} PUBLIC "-framework Cocoa -framework Metal -framework CoreVideo -framework QuartzCore -framework AppKit")
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