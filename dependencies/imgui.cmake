# Dear ImGui vendored as a plain static library target: upstream ships no
# CMakeLists.txt (consumers are expected to compile the .cpp files directly).
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/src/imgui)

set(IMGUI_SRCS
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
)

if (VKM_USE_VULKAN_API OR VKM_USE_WEBGPU_API)
    # The Vulkan and WebGPU backends use GLFW for windowing/input; Metal (Apple) doesn't --
    # its ImGui backend is a custom Metal4 renderer with a native NSEvent input bridge instead.
    list(APPEND IMGUI_SRCS ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp)
endif()

if (VKM_USE_VULKAN_API)
    list(APPEND IMGUI_SRCS ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
endif()

if (VKM_USE_WEBGPU_API)
    list(APPEND IMGUI_SRCS ${IMGUI_DIR}/backends/imgui_impl_wgpu.cpp)
endif()

add_library(imgui STATIC ${IMGUI_SRCS})

target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

if (VKM_USE_VULKAN_API)
    target_link_libraries(imgui PRIVATE volk)
    # vkm loads Vulkan entry points dynamically through volk rather than linking directly
    # against a Vulkan loader; imgui_impl_vulkan.cpp needs to route through the same
    # volk-loaded dispatch table instead of assuming statically-linked vk* symbols.
    target_compile_definitions(imgui PRIVATE IMGUI_IMPL_VULKAN_USE_VOLK)
endif()

if (VKM_USE_VULKAN_API OR VKM_USE_WEBGPU_API)
    target_link_libraries(imgui PRIVATE glfw)
endif()

if (EMSCRIPTEN AND VKM_USE_WEBGPU_API)
    target_compile_options(imgui PRIVATE ${VKM_EMSCRIPTEN_COMPILE_FLAGS})
endif()

set_property(TARGET imgui PROPERTY FOLDER "ThirdPartyLibraries")
