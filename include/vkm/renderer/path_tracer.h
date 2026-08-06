// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>

namespace vkm
{
    class VkmCommandBufferBase;
    class VkmDriverBase;
    class VkmPipelineStateBase;
    class VkmPipelineStateManager;
    class VkmResourceTableBase;

    /*
    * @brief Mirrors PathTraceConstants in resources/Shaders/path_trace.hlsl.
    *
    * Scalars only, so neither side has to reason about vector alignment -- the same rule
    * SceneBatchConstants follows and for the same reason.
    */
    struct VkmPathTraceConstants
    {
        uint32_t _width = 0;
        uint32_t _height = 0;
        uint32_t _sampleIndex = 0;
        uint32_t _maxBounces = 0;
        float    _environmentR = 0.0f;
        float    _environmentG = 0.0f;
        float    _environmentB = 0.0f;
        uint32_t _pad0 = 0;
    };
    static_assert(sizeof(VkmPathTraceConstants) == 32, "VkmPathTraceConstants must match the shader-side struct");

    struct VkmPathTraceOptions
    {
        /*
        * Path length. A path still alive after this many bounces is dropped along with its
        * energy -- there is no Russian roulette, so this is a bias knob, not just a cost one.
        * Four is enough for a convex object in an environment; an enclosed room needs more.
        */
        uint32_t _maxBounces = 4;
        /*
        * Uniform environment radiance returned by a ray that hits nothing. Together with emissive
        * materials it is the whole of this tracer's lighting: `VkmFrameData::_lightDirection` is
        * deliberately ignored, because a directional light has no area and a reference tracer that
        * special-cased one would not be measuring the same integral the techniques are.
        */
        glm::vec3 _environmentRadiance{ 0.0f, 0.0f, 0.0f };
    };

    /*
    * @brief Phase 6's brute-force reference path tracer.
    *
    * @details Ground truth, not a technique: every later phase is measured against what this
    * converges to. It accumulates into one buffer across dispatches, so a caller drives it by
    * calling `recordAccumulate` once per frame and reading the result whenever it likes; `reset()`
    * starts a new average, which is what a moved camera needs.
    *
    * Requires `VkmDriverCapabilityFlags::RayTracing` and a scene whose acceleration structure is
    * published in the bindless set (`VkmScene::buildAccelerationStructures`). It reads the scene
    * entirely through set 0 and set 1 -- objects, materials, geometry pools, the structure and the
    * camera -- so it takes no scene pointer and needs no per-frame binding beyond its own output.
    *
    * The output is a buffer rather than a storage texture because `VkmTableResourceType` has no
    * storage-texture kind; `rgb` is summed radiance and `a` is the number of samples in that sum,
    * so a consumer divides.
    */
    class VkmPathTracer
    {
    public:
        VkmPathTracer() = default;
        ~VkmPathTracer() = default;

        VkmPathTracer(const VkmPathTracer&) = delete;
        VkmPathTracer& operator=(const VkmPathTracer&) = delete;

        /*
        * @brief Creates the accumulation buffer and resolves the pipeline. Fails on a driver
        * without ray tracing rather than degrading, since there is no meaningful fallback.
        *
        * Loads its own PSO directory: ray-tracing pipelines are deliberately not part of the set
        * `VkmEngine` loads at startup, which every backend has to be able to create.
        */
        bool initialize(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager,
                        uint32_t width, uint32_t height, std::string* outError);

        void destroy(VkmDriverBase* driver);

        // Discards the accumulated samples; the next recorded dispatch starts a fresh average.
        inline void reset() { _sampleIndex = 0; }

        /*
        * @brief Records one sample per pixel into the accumulation buffer. Must be recorded into a
        * compute subgraph, after the scene's own update, and outside any render pass.
        *
        * Advances the sample counter immediately, so a caller that records N times has N samples
        * whether or not it waited for any of them.
        */
        void recordAccumulate(VkmCommandBufferBase* commandBuffer, const VkmPathTraceOptions& options);

        inline VkmResourceHandle getAccumulationBuffer() const { return _accumulationBuffer; }
        // How many samples the recorded dispatches have contributed. Equals the alpha channel.
        inline uint32_t getSampleCount() const { return _sampleIndex; }
        inline uint32_t getWidth() const { return _width; }
        inline uint32_t getHeight() const { return _height; }

    private:
        uint32_t _width = 0;
        uint32_t _height = 0;
        uint32_t _sampleIndex = 0;

        VkmResourceHandle _accumulationBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmPipelineStateBase* _pipeline = nullptr;
        VkmResourceTableBase* _resourceTable = nullptr;
    };

    /*
    * @brief Mean squared error between two accumulated images, per RGB component averaged.
    *
    * @details The number later phases are scored with. Both inputs are the path tracer's
    * `float4` layout (rgb summed, a = sample count) and are normalized by their own alpha before
    * comparison, so an image with more samples in it is not penalised for being brighter. Pixels
    * whose sample count is zero on either side are skipped and excluded from the divisor.
    *
    * Returns 0 for an empty comparison, which is also what identical inputs give.
    */
    float vkmComputeImageMse(const float* referenceRgba, const float* candidateRgba, uint32_t pixelCount);

    /*
    * @brief Relative MSE: each pixel's squared error divided by the reference's own squared
    * magnitude.
    *
    * @details The one to quote for a GI comparison. Plain MSE is dominated by whichever part of
    * the image is brightest, so a technique can look good by getting a bright wall right while
    * getting every dim corner wrong. The epsilon in the denominator is what keeps a black
    * reference pixel from making the whole number meaningless.
    */
    float vkmComputeImageRelativeMse(const float* referenceRgba, const float* candidateRgba,
                                     uint32_t pixelCount);
} // namespace vkm
