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

if (NOT VKM_PLATFORM_APPLE)
    # Apple has no GLFW window; its ImGui backend is a custom Metal4 renderer
    # with a native NSEvent input bridge instead.
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
endif()

if (NOT VKM_PLATFORM_APPLE)
    target_link_libraries(imgui PRIVATE glfw)
endif()

if (EMSCRIPTEN AND VKM_USE_WEBGPU_API)
    target_compile_options(imgui PRIVATE ${VKM_EMSCRIPTEN_COMPILE_FLAGS})
endif()

set_property(TARGET imgui PROPERTY FOLDER "ThirdPartyLibraries")
