set(target UnitTests)
set(ROOT_DIR ${PROJECT_SOURCE_DIR})
set(PUBLIC_HDR_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Includes
set(PUBLIC_HDRS 
    ${PUBLIC_HDR_DIR}/UnitTestUnits.hpp
)

# Sources
set(SRCS
    ${SRC_DIR}/UnitTests.cpp
    ${SRC_DIR}/TestEngineSetup.cpp
    ${SRC_DIR}/TestPsoConfig.cpp
    ${SRC_DIR}/TestMemoryTracker.cpp
    ${SRC_DIR}/TestShaderCacheUtil.cpp
    ${SRC_DIR}/TestTextureShaderReadBarrier.cpp
    ${SRC_DIR}/TestRenderGraphBarrierPlan.cpp
    ${SRC_DIR}/TestResourceTables.cpp
    ${SRC_DIR}/TestGBuffer.cpp
    ${SRC_DIR}/TestProbeVolume.cpp
    ${SRC_DIR}/TestSceneCullViews.cpp
    ${SRC_DIR}/TestViewport.cpp
    ${SRC_DIR}/TestPipelineStateManager.cpp
    ${SRC_DIR}/TestBindlessSlotAllocator.cpp
    ${SRC_DIR}/TestPushConstantRingAllocator.cpp
    ${SRC_DIR}/TestInputHandler.cpp
    ${SRC_DIR}/TestTimeBudget.cpp
    ${SRC_DIR}/TestCamera.cpp
    ${SRC_DIR}/TestClipSpaceOrientation.cpp
    ${SRC_DIR}/TestRasterState.cpp
    ${SRC_DIR}/TestGlobalVariable.cpp
    ${SRC_DIR}/TestVertexLayout.cpp
    ${SRC_DIR}/TestObjectDataLayout.cpp
    ${SRC_DIR}/TestSceneGeometryPool.cpp
    ${SRC_DIR}/TestSceneBatching.cpp
    ${SRC_DIR}/TestGltfImporter.cpp
    ${SRC_DIR}/TestCpuProfiler.cpp
    ${SRC_DIR}/TestGpuProfiler.cpp
    ${SRC_DIR}/TestImageLoader.cpp
    ${SRC_DIR}/TestMipChain.cpp
    ${SRC_DIR}/TestCubemapTexture.cpp
    ${SRC_DIR}/TestBufferHostWrite.cpp
    ${SRC_DIR}/TestAccelerationStructure.cpp
    ${SRC_DIR}/TestGBufferRenderVulkan.cpp
    ${SRC_DIR}/TestPathTracerMetrics.cpp
    ${SRC_DIR}/TestReservoirLayout.cpp
)

if (VKM_USE_METAL_API)
    list(APPEND SRCS
        ${SRC_DIR}/TestMetalDriver.mm
        ${SRC_DIR}/TestBackbufferReadback.mm
        ${SRC_DIR}/TestMetalBindlessTriangle.mm
        ${SRC_DIR}/TestRenderGraphCapture.mm
        ${SRC_DIR}/TestGpuProfilerCapture.mm
        ${SRC_DIR}/TestClipSpaceOrientationMetal.mm
        ${SRC_DIR}/TestRasterStateMetal.mm
        ${SRC_DIR}/TestSceneModelRenderMetal.mm
        ${SRC_DIR}/TestCubemapTextureMetal.mm
        ${SRC_DIR}/TestBufferHostWriteMetal.mm
        ${SRC_DIR}/TestTexturePlacementMetal.mm
        ${SRC_DIR}/TestResourceTablesMetal.mm
        ${SRC_DIR}/TestGBufferMetal.mm
        ${SRC_DIR}/TestGBufferRenderMetal.mm
        ${SRC_DIR}/TestProbeVolumeMetal.mm
        ${SRC_DIR}/TestViewportMetal.mm
        ${SRC_DIR}/TestAccelerationStructureMetal.mm
    )
endif()

# Build executable
add_executable(${target} ${SRCS} ${BACKWARD_ENABLE})

if (VKM_USE_METAL_API)
    set_source_files_properties(
        ${SRC_DIR}/TestMetalDriver.mm
        ${SRC_DIR}/TestBackbufferReadback.mm
        ${SRC_DIR}/TestMetalBindlessTriangle.mm
        ${SRC_DIR}/TestRenderGraphCapture.mm
        ${SRC_DIR}/TestClipSpaceOrientationMetal.mm
        ${SRC_DIR}/TestRasterStateMetal.mm
        ${SRC_DIR}/TestSceneModelRenderMetal.mm
        ${SRC_DIR}/TestCubemapTextureMetal.mm
        ${SRC_DIR}/TestBufferHostWriteMetal.mm
        ${SRC_DIR}/TestTexturePlacementMetal.mm
        ${SRC_DIR}/TestResourceTablesMetal.mm
        ${SRC_DIR}/TestGBufferMetal.mm
        ${SRC_DIR}/TestGBufferRenderMetal.mm
        ${SRC_DIR}/TestProbeVolumeMetal.mm
        ${SRC_DIR}/TestViewportMetal.mm
        ${SRC_DIR}/TestAccelerationStructureMetal.mm
        PROPERTIES COMPILE_FLAGS "-x objective-c++"
    )
endif()

# TestMetalBindlessTriangle and TestClipSpaceOrientation* drive the real triangle sample
# PSO; compile its shader caches for this build's backend next to the test binary. Needed
# by both the Metal and Vulkan backends.
if (VKM_USE_METAL_API OR VKM_USE_VULKAN_API)
    vkm_add_shader_cache_target(
        TARGET_NAME   tests_triangle_shaders
        OUTPUT_DIR    "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache"
        PSO_JSON_FILES "${ROOT_DIR}/src/samples/triangle/renderpass.json"
        EXTRA_DEPENDS  "${ROOT_DIR}/src/samples/triangle/triangle.hlsl")
endif()

# TestCubemapTexture* renders the skybox sample's PSO into an offscreen target and reads
# the pixels back, on both backends -- that cross-backend comparison is the point, since it
# is what pins the shared clip-space/ray-reconstruction convention (see the note on
# -fvk-invert-y in skybox.hlsl).
if (VKM_USE_METAL_API OR VKM_USE_VULKAN_API)
    vkm_add_shader_cache_target(
        TARGET_NAME   tests_skybox_shaders
        OUTPUT_DIR    "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache"
        PSO_JSON_FILES "${ROOT_DIR}/src/samples/skybox/renderpass.json"
        EXTRA_DEPENDS  "${ROOT_DIR}/src/samples/skybox/skybox.hlsl")
endif()

# TestResourceTables dispatches compute shaders whose only resources live in the PSO-declared sets
# 2 and 3 -- Vulkan allocates descriptor sets, Metal binds discretely onto the argument table at
# per-set index bases, WebGPU builds bind groups, and the same expected values across all three is
# what proves the PSO declaration means the same thing on each. One PSO declares both sets (so an
# aliased pair is visible); a second declares set 3 only (so a set that slid down to index 2 is).
# WebGPU joins only when a host vkm-compiler can build it a WGSL cache (see the elseif below).
if (VKM_USE_VULKAN_API OR VKM_USE_METAL_API OR (EMSCRIPTEN AND VKM_HOST_VKM_COMPILER))
    vkm_add_shader_cache_target(
        TARGET_NAME   tests_resource_table_shaders
        OUTPUT_DIR    "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache"
        PSO_JSON_FILES "${ROOT_DIR}/resources/tests/resource_tables/renderpass.json"
                       "${ROOT_DIR}/resources/tests/resource_tables/per_draw_only.json"
        EXTRA_DEPENDS  "${ROOT_DIR}/resources/tests/resource_tables/resource_tables.hlsl"
                       "${ROOT_DIR}/resources/tests/resource_tables/per_draw_only.hlsl")
endif()

# TestViewport renders a constant-colour fullscreen triangle into part of an attachment, which is
# the capability the probe update needs to pack several views into one render pass.
if (VKM_USE_VULKAN_API OR VKM_USE_METAL_API)
    vkm_add_shader_cache_target(
        TARGET_NAME   tests_viewport_shaders
        OUTPUT_DIR    "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache"
        PSO_JSON_FILES "${ROOT_DIR}/resources/tests/viewport/renderpass.json"
        EXTRA_DEPENDS  "${ROOT_DIR}/resources/tests/viewport/viewport_fill.hlsl")
endif()

# TestRayQuery casts rays at a scene's acceleration structure through the bindless set. Native
# backends only: WebGPU has no acceleration structure API and never reports the RayTracing
# capability, so a WGSL cache for this would be a cache nothing can ever load.
if (VKM_USE_VULKAN_API OR VKM_USE_METAL_API)
    vkm_add_shader_cache_target(
        TARGET_NAME   tests_ray_query_shaders
        OUTPUT_DIR    "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache"
        PSO_JSON_FILES "${ROOT_DIR}/resources/tests/ray_query/renderpass.json"
        EXTRA_DEPENDS  "${ROOT_DIR}/resources/tests/ray_query/ray_query.hlsl")
endif()

# TestSceneModelRenderMetal draws an imported glTF mesh with the model_viewer sample's PSO.
# It is Metal-only (real-pixel output is only verified on the backend this machine can run,
# like TestMetalBindlessTriangle), so its shader cache is built only for the Metal backend.
if (VKM_USE_METAL_API)
    vkm_add_shader_cache_target(
        TARGET_NAME   tests_model_viewer_shaders
        OUTPUT_DIR    "${CMAKE_CURRENT_BINARY_DIR}/ShaderCache"
        PSO_JSON_FILES "${ROOT_DIR}/src/samples/model_viewer/renderpass.json"
        EXTRA_DEPENDS  "${ROOT_DIR}/src/samples/model_viewer/model_viewer.hlsl")
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
    PRIVATE
    ${ROOT_DIR}/dependencies/src/doctest
    ${ROOT_DIR}/dependencies/src/taskflow
    ${ROOT_DIR}/dependencies/src/stb
    ${ROOT_DIR}/include
)

# Compile options
target_compile_options(${target}
    PRIVATE
    ${DEFAULT_COMPILE_OPTIONS}
)

# Compile definitions
if (EMSCRIPTEN AND VKM_USE_WEBGPU_API)
    # resources/ is preloaded into the Emscripten test binary's MEMFS virtual filesystem
    # (see the --preload-file link option below) under /resources/, not the real on-disk
    # RESOURCES_DIR path, which doesn't exist inside the WASM sandbox.
    set(TEST_RESOURCES_DIR "/resources/")
else()
    set(TEST_RESOURCES_DIR "${RESOURCES_DIR}")
endif()

target_compile_definitions(${target}
    PRIVATE
    RESOURCES_DIR="${TEST_RESOURCES_DIR}"
    ${DEFAULT_COMPILE_DEFINITIONS}
)

if (VKM_USE_METAL_API OR VKM_USE_VULKAN_API)
    add_dependencies(${target} tests_triangle_shaders)
    target_compile_definitions(${target}
        PRIVATE
        TEST_TRIANGLE_SAMPLE_DIR="${ROOT_DIR}/src/samples/triangle/"
        TEST_TRIANGLE_SHADER_CACHE_DIR="${CMAKE_CURRENT_BINARY_DIR}/ShaderCache/"
    )
    add_dependencies(${target} tests_skybox_shaders)
    target_compile_definitions(${target}
        PRIVATE
        TEST_SKYBOX_SAMPLE_DIR="${ROOT_DIR}/src/samples/skybox/"
        TEST_SKYBOX_SHADER_CACHE_DIR="${CMAKE_CURRENT_BINARY_DIR}/ShaderCache/"
    )
endif()

if (VKM_USE_VULKAN_API OR VKM_USE_METAL_API)
    add_dependencies(${target} tests_viewport_shaders)
    target_compile_definitions(${target}
        PRIVATE
        TEST_VIEWPORT_PSO_DIR="${ROOT_DIR}/resources/tests/viewport/"
        TEST_VIEWPORT_SHADER_CACHE_DIR="${CMAKE_CURRENT_BINARY_DIR}/ShaderCache/"
    )
    add_dependencies(${target} tests_resource_table_shaders)
    target_compile_definitions(${target}
        PRIVATE
        TEST_RESOURCE_TABLE_PSO_DIR="${ROOT_DIR}/resources/tests/resource_tables/"
        TEST_RESOURCE_TABLE_SHADER_CACHE_DIR="${CMAKE_CURRENT_BINARY_DIR}/ShaderCache/"
    )
    add_dependencies(${target} tests_ray_query_shaders)
    target_compile_definitions(${target}
        PRIVATE
        TEST_RAY_QUERY_PSO_DIR="${ROOT_DIR}/resources/tests/ray_query/"
        TEST_RAY_QUERY_SHADER_CACHE_DIR="${CMAKE_CURRENT_BINARY_DIR}/ShaderCache/"
    )
elseif (EMSCRIPTEN AND VKM_USE_WEBGPU_API AND VKM_HOST_VKM_COMPILER)
    # The bind-group 2/3 paths on WebGPU need a WGSL shader to exercise them, which only a host
    # vkm-compiler can build.
    # The PSO json rides the resources/ mount; the cache rides the /shadercache one below.
    add_dependencies(${target} tests_resource_table_shaders)
    target_compile_definitions(${target}
        PRIVATE
        TEST_RESOURCE_TABLE_PSO_DIR="/resources/tests/resource_tables/"
        TEST_RESOURCE_TABLE_SHADER_CACHE_DIR="/shadercache/"
    )
endif()

# The engine PSO cache holds the scene's culling / emit compute pipelines, which VkmEngine loads at
# startup on EVERY backend -- a missing cache makes initializeBackendDriver fail outright, so this
# dependency must not be scoped to one backend.
if (TARGET vkm_engine_shaders)
    add_dependencies(${target} vkm_engine_shaders)
endif()

# The path tracer's PSO, which VkmPathTracer::initialize loads from its own directory rather than
# the one VkmEngine loads at startup -- so unlike the above this target exists on native backends
# only, and the dependency is scoped the same way.
if (TARGET vkm_ray_tracing_shaders)
    add_dependencies(${target} vkm_ray_tracing_shaders)
endif()

# TestSceneModelRenderMetal (Metal-only) needs the model_viewer PSO cache and its paths.
if (VKM_USE_METAL_API)
    add_dependencies(${target} tests_model_viewer_shaders)
    target_compile_definitions(${target}
        PRIVATE
        TEST_MODEL_VIEWER_SAMPLE_DIR="${ROOT_DIR}/src/samples/model_viewer/"
        TEST_MODEL_VIEWER_SHADER_CACHE_DIR="${CMAKE_CURRENT_BINARY_DIR}/ShaderCache/"
    )
endif()

# The engine PSO directory and its cache are backend-agnostic -- the cache is built for whichever
# backend is configured -- so these are defined for every backend rather than only Metal. A
# Metal-only test body still has to *compile* in a Vulkan build when it lives in a shared header.
#
# WebGPU gets them too, but only when a host vkm-compiler was supplied: without one
# vkm_add_shader_cache_target() skips, so the cache directory would hold no webgpu entries and
# every lookup would fail at run time rather than compile time. Both directories live under
# resources/, which the --preload-file below maps into MEMFS wholesale, so the paths are the
# virtual ones there.
if (VKM_USE_METAL_API OR VKM_USE_VULKAN_API)
    target_compile_definitions(${target}
        PRIVATE
        TEST_ENGINE_PIPELINE_DIR="${ROOT_DIR}/resources/Pipelines/Engine/"
        TEST_ENGINE_SHADER_CACHE_DIR="${ROOT_DIR}/resources/Shaders/ShaderCache/"
    )
elseif (EMSCRIPTEN AND VKM_USE_WEBGPU_API AND VKM_HOST_VKM_COMPILER)
    target_compile_definitions(${target}
        PRIVATE
        TEST_ENGINE_PIPELINE_DIR="/resources/Pipelines/Engine/"
        TEST_ENGINE_SHADER_CACHE_DIR="/resources/Shaders/ShaderCache/"
    )
endif()

# Link libraries
target_link_libraries(${target}
    PUBLIC
    ${DEFAULT_LINKER_OPTIONS}
	${DEFAULT_LIBRARIES}
    vkmcore
)

if (EMSCRIPTEN AND VKM_USE_WEBGPU_API)
    target_compile_options(${target} PRIVATE ${VKM_EMSCRIPTEN_COMPILE_FLAGS})
    target_link_options(${target} PRIVATE ${VKM_EMSCRIPTEN_PORT_FLAGS})

    # VKM_EMSCRIPTEN_PORT_FLAGS sets ALLOW_MEMORY_GROWTH=1 project-wide, which makes the
    # WASM heap's backing ArrayBuffer "resizable" (a `maximum` is specified to
    # WebAssembly.Memory). Current Chrome/V8 rejects calling TextDecoder.decode() on a
    # resizable ArrayBuffer, which crashes real WebGPU adapter negotiation under headless
    # Chrome specifically in this test binary. UnitTests' heap usage is small and known
    # ahead of time (unlike the sample/vkmcore, which may need to grow), so disable growth
    # here and fix a generous initial size instead — with no `maximum`, the buffer is a
    # plain non-resizable ArrayBuffer, sidestepping the V8 restriction entirely. emcc's
    # `-s` settings take the last-specified value, so this must come after
    # VKM_EMSCRIPTEN_PORT_FLAGS above to win.
    target_link_options(${target} PRIVATE
        "SHELL:-s ALLOW_MEMORY_GROWTH=0"
        "SHELL:-s INITIAL_MEMORY=134217728"
    )

    # Mounts resources/ into the test binary's MEMFS virtual filesystem at /resources/ so
    # fixture-reading tests (e.g. TestPsoConfig.cpp, TestPipelineStateManager.cpp) can find
    # their JSON files under RESOURCES_DIR (see TEST_RESOURCES_DIR above). resources/ is
    # small (tens of KB), so preloading the whole directory is simpler than curating a subset.
    target_link_options(${target} PRIVATE
        "SHELL:--preload-file ${ROOT_DIR}/resources@/resources"
    )

    # Test-owned shader caches are generated into the build tree rather than into resources/,
    # so they need their own mount. Only when a host vkm-compiler produced them: preloading a
    # directory that does not exist fails the link.
    if (VKM_HOST_VKM_COMPILER)
        target_link_options(${target} PRIVATE
            "SHELL:--preload-file ${CMAKE_CURRENT_BINARY_DIR}/ShaderCache@/shadercache"
        )
    endif()
endif()
