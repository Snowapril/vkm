// Copyright (c) 2025 Snowapril
//
// Phase 6's gate: the white furnace test, and energy conservation on a small diffuse scene.
//
// The fixture is `gltf_furnace.gltf` -- two unit cubes in a uniform environment, one with
// baseColorFactor 1.0 and one with 0.5, six units either side of the origin. Both answers are
// **analytic and exact**, not merely convergent, which is what makes this a gate rather than a
// smoke test:
//
//   - A convex diffuse body in a uniform environment of radiance L reflects exactly albedo * L.
//     Convex, so a scattered ray never re-hits the same cube; uniform, so it does not matter where
//     the ray goes.
//   - Cosine-weighted sampling of a Lambertian BRDF makes a bounce's throughput multiplier exactly
//     the albedo, with no pdf division anywhere. The estimator therefore has **zero variance** on
//     this scene: one sample per pixel already gives the exact answer, and more samples must not
//     move it.
//   - Inter-reflection between the two cubes changes nothing. A ray leaving the grey cube that
//     hits the white one still ends at 1.0 * L, so the grey cube reads 0.5 * L either way.
//
// With albedo 1.0 the white cube becomes *invisible*: it returns exactly the environment behind
// it. That is the classic white furnace result, and it means the whole left half of the image has
// to be one value -- an assertion that needs no knowledge of where the cube projects to.
#pragma once

#include "UnitTestUtils.hpp"

#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/path_tracer.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace vkmtest
{
    namespace detail
    {
        // Small enough to keep the test inside its time budget on a software rasterizer, wide
        // enough that each cube covers many pixels at this camera.
        inline constexpr uint32_t kFurnaceWidth = 64;
        inline constexpr uint32_t kFurnaceHeight = 32;
        // Grey, not white, so a channel swap or a dropped channel is visible.
        inline constexpr float kEnvironmentR = 0.40f;
        inline constexpr float kEnvironmentG = 0.60f;
        inline constexpr float kEnvironmentB = 0.80f;
        inline constexpr float kGreyAlbedo = 0.5f;

        // The estimator is exact, so this covers float32 accumulation and the matrix round trip
        // through the camera -- not Monte Carlo noise, of which there is none here.
        inline constexpr float kFurnaceTolerance = 1.0e-3f;

        struct FurnaceImage
        {
            std::vector<float> _rgba; // width * height * 4, rgb summed and a = sample count
            uint32_t _width = 0;
            uint32_t _height = 0;

            // The average at a pixel: what the accumulator holds divided by how many samples are
            // in it. A sample count of zero would mean the dispatch never covered this pixel.
            glm::vec3 average(uint32_t x, uint32_t y) const
            {
                const size_t base = (static_cast<size_t>(y) * _width + x) * 4;
                const float samples = _rgba[base + 3];
                REQUIRE(samples > 0.0f);
                return glm::vec3(_rgba[base + 0], _rgba[base + 1], _rgba[base + 2]) / samples;
            }
        };

        inline void checkNear(const glm::vec3& actual, const glm::vec3& expected, const char* what,
                              uint32_t x, uint32_t y)
        {
            const bool match = std::abs(actual.x - expected.x) <= kFurnaceTolerance &&
                               std::abs(actual.y - expected.y) <= kFurnaceTolerance &&
                               std::abs(actual.z - expected.z) <= kFurnaceTolerance;
            CHECK_MESSAGE(match, what << " at (" << x << ", " << y << ") was ("
                                      << actual.x << ", " << actual.y << ", " << actual.z
                                      << "), expected (" << expected.x << ", " << expected.y << ", "
                                      << expected.z << ")");
        }

        inline FurnaceImage readAccumulation(vkm::VkmDriverBase* driver, const vkm::VkmPathTracer& tracer)
        {
            FurnaceImage image;
            image._width = tracer.getWidth();
            image._height = tracer.getHeight();
            const uint64_t byteSize =
                static_cast<uint64_t>(image._width) * image._height * 4 * sizeof(float);

            vkm::VkmStagingBufferInfo stagingInfo{};
            stagingInfo._flags = vkm::VkmResourceCreateInfo::AllowTransferDst;
            stagingInfo._size = byteSize;
            stagingInfo._debugName = "PathTraceReadback";
            vkm::VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
            REQUIRE(staging != nullptr);

            const vkm::VkmResourceHandle source = tracer.getAccumulationBuffer();
            const vkm::VkmResourceHandle destination = staging->getHandle();
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            auto* subGraph = renderGraph.beginTransferSubGraph("PathTraceReadback");
            subGraph->setTransferCallback([=](vkm::VkmCommandBufferBase* commandBuffer) {
                commandBuffer->copyBuffer(source, destination, 0, 0, byteSize);
            });
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();

            image._rgba.resize(static_cast<size_t>(image._width) * image._height * 4);
            staging->invalidate(0, byteSize);
            std::memcpy(image._rgba.data(), staging->map(), byteSize);
            driver->getRenderResourcePool()->releaseResource(destination);
            return image;
        }
    } // namespace detail

    inline void runPathTracerFurnaceTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);
        if ((driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::RayTracing) == 0)
        {
            MESSAGE("Skipping: this backend reports no RayTracing capability.");
            return;
        }

        vkm::VkmGltfImportOptions importOptions;
        importOptions._optimizeMeshes = false;

        vkm::VkmSceneModel model;
        std::string error;
        REQUIRE_MESSAGE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_furnace.gltf",
                                             &model, &error, importOptions),
                        error);

        vkm::VkmPipelineStateManager manager(driver);
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR,
                                                                TEST_ENGINE_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::Engine, &error),
                        error);

        vkm::VkmScene scene;
        REQUIRE(scene.addModel(model, &error));
        REQUIRE_MESSAGE(scene.build(driver, &manager, &error), error);
        REQUIRE(scene.getObjects().size() == 2);
        REQUIRE_MESSAGE(scene.buildAccelerationStructures(driver, &error), error);

        REQUIRE_MESSAGE(vkm::vkmLoadRayTracingPipelineStates(&manager, &error), error);

        vkm::VkmPathTracer tracer;
        REQUIRE_MESSAGE(tracer.initialize(driver, &manager, detail::kFurnaceWidth,
                                          detail::kFurnaceHeight, &error),
                        error);

        // Down -Z from +Z, with both cubes in frame: the white one sits in the left half and the
        // grey one in the right. A 2:1 aspect at 60 degrees vertical covers x out to about +/-16
        // at this distance, so neither unit cube can spill across the midline.
        const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 24.0f), glm::vec3(0.0f),
                                             glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 projection = glm::perspectiveRH_ZO(
            glm::radians(60.0f),
            static_cast<float>(detail::kFurnaceWidth) / static_cast<float>(detail::kFurnaceHeight),
            0.1f, 200.0f);
        const glm::mat4 viewProjection = projection * view;

        // The tracer builds its primary rays from inverseViewProjection alone, so this is the only
        // camera state it reads -- and getting the inverse wrong would put every ray somewhere the
        // geometry is not, which the furnace test cannot distinguish from a missing scene. That is
        // what the grey cube's presence check covers.
        vkm::VkmFrameConstants frameConstants{};
        frameConstants._view = view;
        frameConstants._projection = projection;
        frameConstants._viewProjection = viewProjection;
        frameConstants._inverseViewProjection = glm::inverse(viewProjection);
        frameConstants._prevViewProjection = viewProjection;
        frameConstants._cameraPositionWorld = glm::vec4(0.0f, 0.0f, 24.0f, 1.0f);
        driver->getFrameConstantManager()->update(/*frameIndex=*/0, frameConstants);

        // recordUpdate() overwrites _materialPoolSlot from the scene's own registration, so the
        // only field that matters here is the frustum -- and the tracer ignores it, since a ray
        // is not culled.
        vkm::VkmFrameData frameData;
        vkm::vkmExtractFrustumPlanes(viewProjection, frameData._frustumPlanes);

        // The path tracer reads the camera from set 1 and everything else from set 0, so the only
        // per-frame work is publishing the scene's own records.
        std::vector<vkm::VkmResourceHandle> referenced;
        scene.collectReferencedResources(&referenced);
        referenced.push_back(tracer.getAccumulationBuffer());
        referenced.push_back(scene.getTopLevelAccelerationStructure());

        const vkm::VkmPathTraceOptions options{
            /*_maxBounces=*/4,
            /*_jitterPrimaryRay=*/true,
            glm::vec3(detail::kEnvironmentR, detail::kEnvironmentG, detail::kEnvironmentB)
        };

        // Four samples rather than one: the estimator is exact, so this is what shows the
        // accumulator averages rather than sums, and that the sample index reaches the shader.
        constexpr uint32_t kSampleCount = 4;
        for (uint32_t sample = 0; sample < kSampleCount; ++sample)
        {
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);

            auto* updateSubGraph = renderGraph.beginTransferSubGraph("FurnaceSceneUpdate");
            for (vkm::VkmResourceHandle handle : referenced) { updateSubGraph->addReferencedResource(handle); }
            updateSubGraph->setTransferCallback([&scene, frameData](vkm::VkmCommandBufferBase* commandBuffer) {
                scene.recordUpdate(commandBuffer, /*frameIndex=*/0, frameData, /*viewIndex=*/0);
            });

            auto* traceSubGraph = renderGraph.beginComputeSubGraph("FurnacePathTrace");
            for (vkm::VkmResourceHandle handle : referenced) { traceSubGraph->addReferencedResource(handle); }
            traceSubGraph->setComputeCallback([&tracer, options](vkm::VkmCommandBufferBase* commandBuffer) {
                tracer.recordAccumulate(commandBuffer, options);
            });

            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();
        }
        CHECK(tracer.getSampleCount() == kSampleCount);

        const detail::FurnaceImage image = detail::readAccumulation(driver, tracer);
        const glm::vec3 environment(detail::kEnvironmentR, detail::kEnvironmentG, detail::kEnvironmentB);

        SUBCASE("white furnace: an albedo-1 body is invisible against its environment")
        {
            // Every pixel of the left half, cube and background alike. Energy lost at a bounce
            // darkens the cube below the background; energy created brightens it.
            for (uint32_t y = 0; y < image._height; ++y)
            {
                for (uint32_t x = 0; x < image._width / 2; ++x)
                {
                    const glm::vec3 value = image.average(x, y);
                    if (std::abs(value.x - environment.x) > detail::kFurnaceTolerance ||
                        std::abs(value.y - environment.y) > detail::kFurnaceTolerance ||
                        std::abs(value.z - environment.z) > detail::kFurnaceTolerance)
                    {
                        // Report the first offender rather than 1024 separate failures.
                        detail::checkNear(value, environment, "white furnace pixel", x, y);
                        return;
                    }
                }
            }
            CHECK(true);
        }

        SUBCASE("a diffuse body reflects exactly its albedo times the environment")
        {
            /*
            * Stated as a ratio to the environment rather than an absolute colour, because a
            * silhouette pixel is a genuine average of the two: with the primary ray jittered
            * inside its pixel, a pixel half-covered by the cube reads (0.5 + 1.0) / 2 = 0.75 of
            * the environment, which is correct and not something to assert against.
            *
            * So: every pixel is somewhere between the albedo and the environment, the darkest one
            * IS the albedo -- that is the cube's interior, where every sample hit it -- and every
            * channel agrees on the ratio, which is what a swapped or dropped channel breaks.
            */
            float darkestRatio = 1.0f;
            for (uint32_t y = 0; y < image._height; ++y)
            {
                for (uint32_t x = image._width / 2; x < image._width; ++x)
                {
                    const glm::vec3 value = image.average(x, y);
                    const glm::vec3 ratio = value / environment;
                    const bool uniform =
                        std::abs(ratio.x - ratio.y) <= detail::kFurnaceTolerance &&
                        std::abs(ratio.x - ratio.z) <= detail::kFurnaceTolerance;
                    const bool inRange = ratio.x >= detail::kGreyAlbedo - detail::kFurnaceTolerance &&
                                         ratio.x <= 1.0f + detail::kFurnaceTolerance;
                    // One variable, because doctest cannot decompose a `&&` expression.
                    const bool ok = uniform && inRange;
                    if (!ok)
                    {
                        CHECK_MESSAGE(ok,
                                      "grey furnace pixel at (" << x << ", " << y << ") has ratio ("
                                          << ratio.x << ", " << ratio.y << ", " << ratio.z
                                          << "); expected one value in ["
                                          << detail::kGreyAlbedo << ", 1]");
                        return;
                    }
                    darkestRatio = std::min(darkestRatio, ratio.x);
                }
            }
            // The cube has to actually be on screen and fully cover at least one pixel, or the
            // loop above passes by covering nothing but background.
            CHECK(darkestRatio == doctest::Approx(detail::kGreyAlbedo).epsilon(detail::kFurnaceTolerance));
        }

        SUBCASE("no pixel exceeds the environment radiance")
        {
            // Energy conservation stated directly: with no emissive geometry anywhere, nothing in
            // the scene may return more light than was put into it.
            float brightest = 0.0f;
            for (uint32_t y = 0; y < image._height; ++y)
            {
                for (uint32_t x = 0; x < image._width; ++x)
                {
                    const glm::vec3 value = image.average(x, y);
                    brightest = std::max({ brightest, value.x / environment.x,
                                           value.y / environment.y, value.z / environment.z });
                }
            }
            CHECK(brightest <= 1.0f + detail::kFurnaceTolerance);
        }

        tracer.destroy(driver);
        scene.destroy(driver);
    }

} // namespace vkmtest
