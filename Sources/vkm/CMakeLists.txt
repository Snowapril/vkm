# Target name
set(target vkmcore)
set(ROOT_DIR ${PROJECT_SOURCE_DIR})
set(LIBRARY_DIR ${PROJECT_SOURCE_DIR}/Dependencies/src)
set(PUBLIC_HDR_DIR ${PROJECT_SOURCE_DIR}/Includes)
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../)

# Set public headers
set(PUBLIC_HDRS
    ${PUBLIC_HDR_DIR}/vkm/base/platform.h
    ${PUBLIC_HDR_DIR}/vkm/base/common.h
    ${PUBLIC_HDR_DIR}/vkm/base/logger.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/driver.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/backend/common/swapchain.h
    ${PUBLIC_HDR_DIR}/vkm/renderer/engine.h
)

# Set Sources
set(SRCS
    ${SRC_DIR}/vkm/base/common.cpp
    ${SRC_DIR}/vkm/base/logger.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/driver.cpp
    ${SRC_DIR}/vkm/renderer/backend/common/swapchain.cpp
    ${SRC_DIR}/vkm/renderer/engine.cpp
)

if (VKM_USE_VULKAN_API)
    # list(APPEND PUBLIC_HDRS
    #     ${PUBLIC_HDR_DIR}/vkm/renderer/backend/vulkan/vulkan_swapchain.h
    # )
    # list(APPEND SRCS
    #     ${SRC_DIR}/vkm/renderer/backend/vulkan/vulkan_swapchain.cpp
    # )
endif()

if (VKM_USE_METAL_API)
    list(APPEND PUBLIC_HDRS
        $(PUBLIC_HDR_DIR)/vkm/platform/apple/application.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_driver.h
        ${PUBLIC_HDR_DIR}/vkm/renderer/backend/metal/metal_swapchain.h
    )
    list(APPEND SRCS
        ${SRC_DIR}/vkm/platform/apple/application.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_driver.mm
        ${SRC_DIR}/vkm/renderer/backend/metal/metal_swapchain.mm
    )
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