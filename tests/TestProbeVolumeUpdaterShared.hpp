#ifndef TEST_PROBE_VOLUME_UPDATER_SHARED_HPP
#define TEST_PROBE_VOLUME_UPDATER_SHARED_HPP

#include <doctest/doctest.h>

#include "TestHalfFloatShared.hpp"

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/probe_volume.h>
#include <vkm/renderer/probe_volume_updater.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>

#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace vkmtest
{
    /*
    * @brief The analytic convergence model, which is what turns a budget into a number.
    */
    inline void runProbeConvergenceModelTest()
    {
        // A round is ceil(probeCount / budget) frames, and each refresh retains `hysteresis` of the
        // old value, so reaching an error of `f` takes ceil(log f / log hysteresis) refreshes.
        CHECK(vkm::VkmProbeVolumeUpdater::framesToConverge(4, 1, 0.5f, 0.1f) == 16);  // 4 * 4
        CHECK(vkm::VkmProbeVolumeUpdater::framesToConverge(4, 2, 0.5f, 0.1f) == 8);   // 2 * 4
        CHECK(vkm::VkmProbeVolumeUpdater::framesToConverge(4, 1, 0.5f, 0.5f) == 4);   // 4 * 1

        // Zero hysteresis lands on the new value in one refresh, so convergence is one round.
        CHECK(vkm::VkmProbeVolumeUpdater::framesToConverge(2048, 32, 0.0f, 0.1f) == 64);

        // The shipping defaults, and the reason this tier's propagation latency is a documented
        // weak point rather than a footnote: 2048 probes at 32 per frame is a 64-frame round, and
        // hysteresis 0.97 needs 76 refreshes to shed 90% of the error.
        CHECK(vkm::VkmProbeVolumeUpdater::framesToConverge(2048, 32, 0.97f, 0.1f) == 4864);
        MESSAGE("Probe GI propagation at the shipping defaults (2048 probes, budget 32, "
                "hysteresis 0.97): 4864 frames to 90% (about 81 s at 60 Hz), "
                << vkm::VkmProbeVolumeUpdater::framesToConverge(2048, 32, 0.97f, 0.5f)
                << " frames to 50%.");
    }

// Everything below loads a pipeline, and the engine PSO directory is defined for the native
// backends only -- WebGPU builds no shader cache because it has no WGSL compiler (see TODO.md).
#if defined(TEST_ENGINE_PIPELINE_DIR)

    /*
    * @brief The scene, volume and updater the two tests below share.
    *
    * A deliberately tiny grid: four probes, one refreshed per frame, so a round is four frames and
    * a convergence is a handful of them. The point of these tests is the *cadence*, which does not
    * get truer with more probes -- it only gets slower.
    */
    struct ProbeUpdaterFixture
    {
        static constexpr uint32_t kProbeCountX = 2;
        static constexpr uint32_t kProbeCountY = 1;
        static constexpr uint32_t kProbeCountZ = 2;
        static constexpr uint32_t kProbeCount = kProbeCountX * kProbeCountY * kProbeCountZ;

        vkm::VkmDriverBase* driver = nullptr;
        vkm::VkmPipelineStateManager manager;
        vkm::VkmSceneModel model;
        vkm::VkmScene scene;
        vkm::VkmProbeVolume volume;
        vkm::VkmProbeVolumeUpdater updater;

        explicit ProbeUpdaterFixture(vkm::VkmDriverBase* driver, uint32_t budget, float hysteresis)
            : driver(driver), manager(driver)
        {
            std::string error;
            REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_ENGINE_PIPELINE_DIR,
                                                                    TEST_ENGINE_SHADER_CACHE_DIR,
                                                                    vkm::VkmPipelineStateOrigin::Engine, &error),
                            error);

            vkm::VkmGltfImportOptions importOptions;
            importOptions._optimizeMeshes = false;
            REQUIRE(vkm::importGltfModel(std::string(RESOURCES_DIR) + "tests/gltf_triangle.gltf",
                                         &model, &error, importOptions));
            REQUIRE(scene.addModel(model, &error));
            REQUIRE(scene.build(driver, &manager, &error));
            scene.setObjectTransform(0, glm::mat4(1.0f));

            // The fixture triangle spans x,y in [0,1] at z = 0 with +Z normals, so probes placed in
            // front of it on -Z all see it through their +Z face.
            vkm::VkmProbeVolume::Descriptor volumeDescriptor{};
            volumeDescriptor._probeCounts = glm::uvec3(kProbeCountX, kProbeCountY, kProbeCountZ);
            volumeDescriptor._spacing = glm::vec3(1.0f, 1.0f, 1.0f);
            volumeDescriptor._origin = glm::vec3(0.0f, 0.5f, -2.0f);
            REQUIRE(volume.initialize(driver, volumeDescriptor));

            vkm::VkmProbeVolumeUpdater::Descriptor updaterDescriptor{};
            updaterDescriptor._budget = budget;
            updaterDescriptor._hysteresis = hysteresis;
            REQUIRE_MESSAGE(updater.initialize(driver, &manager, &volume, updaterDescriptor, &error), error);
        }

        ~ProbeUpdaterFixture()
        {
            updater.destroy();
            volume.destroy();
            // VkmScene's destructor is defaulted, so its buffers only go back to the pool here.
            // Vulkan's allocator aborts at teardown on anything still outstanding, which is how
            // this was noticed.
            scene.destroy(driver);
        }

        // lightDirection points TOWARDS the light, the engine's convention, so (0,0,1) saturates
        // the triangle's nDotL and (0,0,-1) leaves it unlit.
        vkm::VkmFrameData makeFrameData(const glm::vec3& lightDirection) const
        {
            vkm::VkmFrameData frameData;
            frameData._lightDirection = glm::vec4(lightDirection, 0.0f);
            return frameData;
        }
    };

    /*
    * @brief The round-robin schedule refreshes every probe exactly once per round.
    *
    * @details The budget deliberately does not divide the probe count, which is where the obvious
    * implementation goes wrong: advancing the cursor by a fixed budget and wrapping would refresh
    * some probes twice a round and others not at all. A probe that is never reached is invisible in
    * an image -- it just stays dark -- so this is worth asserting directly rather than inferring
    * from a converged picture.
    *
    * Nothing here is executed: record() only builds subgraphs, and the schedule is decided while it
    * does so.
    */
    inline void runProbeUpdateScheduleTest(vkm::VkmDriverBase* driver)
    {
        constexpr uint32_t kBudget = 3; // 3 does not divide 4
        ProbeUpdaterFixture fixture(driver, kBudget, /*hysteresis=*/0.5f);

        CHECK(fixture.updater.getRoundLengthInFrames() == 2); // ceil(4 / 3)

        const vkm::VkmFrameData frameData = fixture.makeFrameData(glm::vec3(0.0f, 0.0f, 1.0f));
        const auto recordOneFrame = [&]() {
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            fixture.updater.record(&renderGraph, &fixture.scene, frameData);

            std::vector<uint32_t> refreshed;
            for (uint32_t slot = 0; slot < fixture.updater.getUpdateCount(); ++slot)
            {
                refreshed.push_back(fixture.updater.getProbeIndexForSlot(slot));
            }
            renderGraph.reset();
            return refreshed;
        };

        SUBCASE("a round covers every probe exactly once")
        {
            std::vector<uint32_t> covered;
            for (uint32_t frame = 0; frame < fixture.updater.getRoundLengthInFrames(); ++frame)
            {
                const std::vector<uint32_t> refreshed = recordOneFrame();
                covered.insert(covered.end(), refreshed.begin(), refreshed.end());
            }

            // Exactly once: as many refreshes as probes, and no duplicates among them.
            CHECK(covered.size() == ProbeUpdaterFixture::kProbeCount);
            const std::set<uint32_t> unique(covered.begin(), covered.end());
            CHECK(unique.size() == ProbeUpdaterFixture::kProbeCount);
        }

        SUBCASE("the last frame of a round is clamped rather than wrapped")
        {
            CHECK(recordOneFrame().size() == 3);
            // Wrapping would have refreshed 3 probes here too, two of them for the second time.
            CHECK(recordOneFrame().size() == 1);
            CHECK(recordOneFrame().size() == 3); // the next round starts clean
        }
    }

    /*
    * @brief Measures how a light change actually propagates into the atlas, frame by frame.
    *
    * @details This is the measurement Phase 4 exists to produce: the low-spec tier trades rays for
    * amortized rasterization, and the bill for that trade is convergence time. The decay is
    * geometric -- a probe retains `hysteresis` of its old value per refresh, and is refreshed once
    * per round -- so the test asserts the *ratio* rather than any particular value. A ratio is
    * independent of what the probe is looking at and of half-float precision in the atlas, which
    * makes it a much sharper check than "the picture got darker".
    *
    * It is also the assertion that would catch the hardware blend being wired up wrong: with the
    * wrong blend factors the atlas would either jump straight to the new value (ratio 0) or never
    * move (ratio 1), and both still produce a plausible-looking image.
    */
    inline void runProbeGiPropagationTest(vkm::VkmDriverBase* driver)
    {
        constexpr float kHysteresis = 0.5f;
        constexpr uint32_t kBudget = 1; // one probe per frame, so a round is kProbeCount frames
        ProbeUpdaterFixture fixture(driver, kBudget, kHysteresis);

        const uint32_t roundLength = fixture.updater.getRoundLengthInFrames();
        REQUIRE(roundLength == ProbeUpdaterFixture::kProbeCount);

        const glm::uvec2 cellOrigin = fixture.volume.getIrradianceProbeTexelOrigin(0);
        const uint32_t cellCentre =
            (fixture.volume.getDescriptor()._irradianceResolution + 2u * vkm::VkmProbeVolume::kBorderTexels) / 2u;

        const auto renderFrame = [&](const glm::vec3& lightDirection) {
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
            fixture.updater.record(&renderGraph, &fixture.scene, fixture.makeFrameData(lightDirection));
            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();
        };

        // Red channel of probe 0's octahedral centre, which points at the triangle.
        const auto readProbe0 = [&]() {
            const vkm::VkmTextureReadbackResult readback =
                driver->readbackTexture(fixture.volume.getIrradianceTexture());
            REQUIRE(readback.channels == 8); // bytes per texel, RGBA16F
            const size_t texel = (static_cast<size_t>(cellOrigin.y + cellCentre) * readback.width +
                                  (cellOrigin.x + cellCentre)) *
                                 readback.channels;
            return readHalfComponent(&readback.pixels[texel], 0);
        };

        const glm::vec3 lit(0.0f, 0.0f, 1.0f);
        const glm::vec3 unlit(0.0f, 0.0f, -1.0f);

        // One full round under the light, so every probe has taken its first refresh. A first
        // refresh ignores hysteresis, so the atlas holds the capture exactly and the starting point
        // of the decay below is not itself still converging.
        for (uint32_t frame = 0; frame < roundLength; ++frame)
        {
            renderFrame(lit);
        }
        const float litValue = readProbe0();
        REQUIRE_MESSAGE(litValue > 0.01f, "the probe recorded no light to decay from");

        // Now take the light away. Probe 0 refreshes once per round, and each refresh should retain
        // exactly `hysteresis` of the remaining difference.
        constexpr uint32_t kMeasuredRefreshes = 4;
        std::vector<float> values;
        uint32_t framesSinceChange = 0;
        uint32_t framesToNinetyPercent = 0;
        for (uint32_t refresh = 0; refresh < kMeasuredRefreshes; ++refresh)
        {
            for (uint32_t frame = 0; frame < roundLength; ++frame)
            {
                renderFrame(unlit);
                ++framesSinceChange;
            }
            values.push_back(readProbe0());
            // The unlit value is 0, so the remaining error is the value itself.
            if (framesToNinetyPercent == 0 && values.back() <= 0.1f * litValue)
            {
                framesToNinetyPercent = framesSinceChange;
            }
        }

        // Geometric decay: after k refreshes the probe retains hysteresis^k of the original value.
        // Compared as a fraction of the lit value rather than absolutely, because doctest's Approx
        // folds a scale of 1.0 into its epsilon -- against the small radiances an 8x8 probe records,
        // an absolute comparison at any useful epsilon passes no matter what the atlas holds.
        for (uint32_t refresh = 0; refresh < kMeasuredRefreshes; ++refresh)
        {
            const float expected = std::pow(kHysteresis, static_cast<float>(refresh + 1));
            CHECK(values[refresh] / litValue == doctest::Approx(expected).epsilon(0.1));
        }

        // The same claim without reference to the absolute value, so neither the scene's brightness
        // nor the atlas's half-float precision can flatter it.
        for (uint32_t refresh = 1; refresh < kMeasuredRefreshes; ++refresh)
        {
            CHECK(values[refresh] / values[refresh - 1] == doctest::Approx(kHysteresis).epsilon(0.1));
        }

        // And the measured latency agrees with the model the defaults are projected through. The
        // model is an upper bound: a probe refreshed just before the change waits a whole round for
        // its first refresh, so the true figure sits within one round below it.
        const uint32_t predicted = vkm::VkmProbeVolumeUpdater::framesToConverge(
            ProbeUpdaterFixture::kProbeCount, kBudget, kHysteresis, 0.1f);
        REQUIRE(framesToNinetyPercent > 0);
        CHECK(framesToNinetyPercent <= predicted);
        CHECK(framesToNinetyPercent > predicted - roundLength);
        MESSAGE("Probe GI propagation measured: " << framesToNinetyPercent
                << " frames to 90% (model predicts at most " << predicted << ").");
    }
#endif // TEST_ENGINE_PIPELINE_DIR
} // namespace vkmtest

#endif // TEST_PROBE_VOLUME_UPDATER_SHARED_HPP
