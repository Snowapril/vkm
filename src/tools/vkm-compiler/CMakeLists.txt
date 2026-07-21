set(target vkm-compiler)
set(ROOT_DIR ${PROJECT_SOURCE_DIR})
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Sources
set(SRCS
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/subprocess.cpp
)

# Build executable (a native host tool; never an Emscripten binary)
add_executable(${target} ${SRCS})

# Project options
set_target_properties(${target}
    PROPERTIES
    ${DEFAULT_PROJECT_OPTIONS}
)

# Include directories
target_include_directories(${target}
    PRIVATE
    ${SRC_DIR}
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
    VKM_DXC_EXECUTABLE_PATH="${VKM_DXC_EXECUTABLE}"
    ${DEFAULT_COMPILE_DEFINITIONS}
)

if (VKM_COMPILER_ENABLE_WGSL)
    target_compile_definitions(${target}
        PRIVATE
        VKM_COMPILER_SUPPORTS_WGSL=1
        VKM_TINT_EXECUTABLE_PATH="${VKM_TINT_EXECUTABLE}"
    )
endif()

# On Apple, vkm-compiler compiles spirv-cross MSL into a metallib via the Metal
# toolchain (invoked through xcrun) so the stored .vfcache is a precompiled binary
# library. xcrun ships with the macOS command-line tools and resolves the active
# Metal toolchain at run time, so bake its path rather than a version-specific
# metal binary that goes stale on toolchain updates.
if (APPLE)
    find_program(VKM_XCRUN_EXECUTABLE xcrun)
    if (VKM_XCRUN_EXECUTABLE)
        target_compile_definitions(${target}
            PRIVATE
            VKM_METAL_TOOLS_AVAILABLE=1
            VKM_XCRUN_EXECUTABLE_PATH="${VKM_XCRUN_EXECUTABLE}"
        )
    else()
        message(WARNING "vkm-compiler: xcrun not found; Metal shader caches will store "
                        "MSL source instead of a precompiled metallib")
    endif()
endif()

# Link libraries. Explicitly link both spirv-cross static libs rather than
# relying on transitive propagation from spirv-cross's own private linkage.
target_link_libraries(${target}
    PRIVATE
    ${DEFAULT_LINKER_OPTIONS}
    ${DEFAULT_LIBRARIES}
    vkmcore
    spirv-cross-core
    spirv-cross-msl
)

# On Apple, dxc is built from source via ExternalProject; make sure that build
# finishes before vkm-compiler (which bakes in its path) is linked/used.
if (APPLE AND TARGET dxc_build)
    add_dependencies(${target} dxc_build)
endif()

# When WGSL support is on, Tint is built from the Dawn source tree the same way.
if (VKM_COMPILER_ENABLE_WGSL AND TARGET dawn_tint_build)
    add_dependencies(${target} dawn_tint_build)
endif()
