// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string>

namespace vkm
{
    class VkmCommandBufferBase;
    class VkmDriverBase;
    class VkmGBuffer;
    class VkmPipelineStateBase;
    class VkmPipelineStateManager;
    class VkmResourceTableBase;
    class VkmStagingBuffer;

    // Mirrors VKM_RESERVOIR_WORD_STRIDE in vkm_reservoir.hlsli. TestReservoirLayout pins them.
    inline constexpr uint32_t kVkmReservoirWordStride = 8;
    inline constexpr uint32_t kVkmReservoirByteStride = kVkmReservoirWordStride * 4;

    /*
    * @brief How many screen-sized reservoir slices the buffer holds.
    * @details Three, which is what resampling needs: slice 2 holds the frame's fresh 1-spp
    * reservoirs, and slices 0/1 are a parity-selected history pair — temporal reuse reads last
    * frame's output from one and writes this frame's into the other. Keeping the fresh slice
    * separate is what lets the final-shading MIS blend read it after resampling has run.
    * Not FRAME_COUNT. A naive per-frame slice would need one per frame in flight, but
    * `VkmRenderGraph` calls `ensureCompleted()` on a frame slot before recording into it again,
    * so by the time frame N records, the frame that last wrote these slices has been waited on.
    * The buffer is therefore sized by what the resampling passes need rather than by how many
    * frames are in flight.
    */
    inline constexpr uint32_t kVkmReservoirSliceCount = 3;

    // Mirrors RestirConstants in the reservoir shaders. Scalars only, like every other push-
    // constant record in the engine.
    struct VkmRestirConstants
    {
        uint32_t _width = 0;
        uint32_t _height = 0;
        uint32_t _sampleIndex = 0;
        uint32_t _maxBounces = 0;
        float    _environmentR = 0.0f;
        float    _environmentG = 0.0f;
        float    _environmentB = 0.0f;
        uint32_t _outputSlice = 0;
        uint32_t _inputSlice = 0;
        uint32_t _neighbourCount = 0;
        float    _neighbourRadius = 0.0f;
        // Cosine of the largest angle between two surfaces still considered the same one.
        float    _normalThreshold = 0.0f;
        // Relative camera-distance difference still considered the same surface.
        float    _depthThreshold = 0.0f;
        // Slice the temporal pass reads last frame's reservoirs from.
        uint32_t _historySlice = 0;
        // Largest confidence (M) a history reservoir may carry into a merge.
        uint32_t _confidenceCap = 0;
        // Frames a sample may be reused before it is discarded as stale.
        uint32_t _maxSampleAge = 0;
    };
    static_assert(sizeof(VkmRestirConstants) == 64, "VkmRestirConstants must match the shader-side struct");

    /*
    * @brief Phase 8.2: how many precomputed neighbour offsets the lookup table holds.
    *
    * A power of two, because the spatial pass indexes it with a mask rather than a modulo -- which
    * is the whole reason it is a table and not a per-pixel disk sample.
    */
    inline constexpr uint32_t kVkmNeighbourOffsetCount = 256;

    /*
    * @brief Fills a buffer with low-discrepancy points in the unit disk.
    * @details The R2 sequence mapped through Shirley-Chiu's concentric square-to-disk map. R2
    * rather than a golden-angle spiral because the spatial pass takes a run of consecutive entries,
    * and a spiral's consecutive points share almost the same radius, so a run would sample a ring
    * rather than a disk. Concentric rather than the polar map, which bunches points towards the
    * centre where a neighbour is least useful. Free-standing and driver-free, so the distribution
    * is testable without a GPU.
    * @param outOffsets Receives `offsetCount` xy pairs.
    * @param offsetCount Points to generate, normally kVkmNeighbourOffsetCount.
    */
    void vkmBuildNeighbourOffsets(float* outOffsets, uint32_t offsetCount);

    struct VkmRestirOptions
    {
        // Scatters from the G-buffer surface, matching VkmIndirectOptions: a reference run needs
        // one more, because its first bounce is the primary ray neither pass casts.
        uint32_t _maxBounces = 3;
        glm::vec3 _environmentRadiance{ 0.0f, 0.0f, 0.0f };
        /*
        * Whether the spatial pass runs between generation and resolve. Off reproduces Phase 7's
        * 1-spp estimator exactly, which is what makes 8.3's gate an equality test; on is 8.4,
        * verified against the reference on the Cornell gate. Default off so the un-resampled
        * estimator stays one flag away as a validation mode; a live renderer turns it on.
        */
        bool _spatialResampling = false;
        /*
        * Whether the temporal pass runs between generation and spatial/resolve. When set, the
        * slice routing is the pass's own — generation writes the fresh slice and temporal
        * ping-pongs the history pair by sample parity — and `_inputSlice`/`_outputSlice` below
        * are ignored. Off keeps the options-driven routing byte for byte, which is what the
        * 8.3/8.4 gates measure through.
        */
        bool _temporalResampling = false;
        // Largest confidence a history reservoir may carry into a merge (restir.md section 9:
        // 5-30, start 20). Without it new samples get exponentially negligible weight.
        uint32_t _confidenceCap = 20;
        // Frames a sample may be reused before it is discarded: cached radiance goes stale even
        // where the G-buffer says the surface is the same.
        uint32_t _maxSampleAge = 32;
        // Neighbours merged per pixel, and how far away they are looked for, in pixels. The plan
        // suggests 3-5 at roughly 30 px for a 1080p frame; scale the radius with the resolution.
        uint32_t _neighbourCount = 4;
        float _neighbourRadius = 8.0f;
        // Rejection thresholds, both G-buffer only. 25 degrees and 10% relative depth.
        float _normalThreshold = 0.906f;
        float _depthThreshold = 0.1f;
        /*
        * Which slice holds the freshly generated reservoirs and which holds the resampled ones.
        * They must differ when `_spatialResampling` is set: spatial reuse reads its neighbours
        * from the slice it is not writing, and aliasing them would resample results the same pass
        * had already produced.
        */
        uint32_t _inputSlice = 0;
        uint32_t _outputSlice = 1;
    };

    // Mirrors RestirLightingConstants in gi_restir_lighting.hlsl.
    struct VkmRestirLightingConstants
    {
        // x = resampled slice, y = fresh slice, z = debug view, w = flags (reserved).
        glm::uvec4 _slices{ 0u, 0u, 0u, 0u };
        // x = MIS blend toward the fresh sample on smooth surfaces (0 = off),
        // y = reserved, z = 1 / (confidence cap + 1), w = 1 / max sample age.
        glm::vec4 _params{ 0.0f, 0.0f, 0.0f, 0.0f };
    };
    static_assert(sizeof(VkmRestirLightingConstants) == 32,
                  "VkmRestirLightingConstants must match the shader-side struct");

    // What gi_restir_lighting draws: the lit estimate, or one of the reservoir's bookkeeping
    // fields as a grey ramp -- the caps are invisible in the lit image.
    enum class VkmRestirDebugView : uint32_t
    {
        Lighting = 0,
        Confidence = 1,
        Age = 2,
        Weight = 3,
    };

    /*
    * @brief The ReSTIR GI passes: a reservoir buffer, one traced sample per pixel written into it,
    * and shading from it.
    * @details No resampling yet, which is what makes this step checkable. With a single candidate,
    * RIS reduces to the estimator it resamples -- `W = 1/p_source` -- so resolving a freshly
    * generated reservoir must reproduce `VkmIndirectPass` sample for sample. The only thing that
    * can separate them is the reservoir round trip.
    * The accumulation buffer has the same layout `VkmPathTracer` and `VkmIndirectPass` use (rgb
    * summed, a = sample count), so `vkmComputeImageMse` compares all three directly.
    */
    class VkmRestirPass
    {
    public:
        VkmRestirPass() = default;
        ~VkmRestirPass() = default;

        VkmRestirPass(const VkmRestirPass&) = delete;
        VkmRestirPass& operator=(const VkmRestirPass&) = delete;

        /*
        * @brief Creates the reservoir and accumulation buffers and binds `gbuffer`'s targets.
        * Fails without ray tracing, and on a manager `vkmLoadRayTracingPipelineStates` has not
        * been called on. `gbuffer` must outlive this pass.
        */
        bool initialize(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager,
                        const VkmGBuffer& gbuffer, uint32_t width, uint32_t height,
                        std::string* outError);

        void destroy(VkmDriverBase* driver);

        inline void reset() { _sampleIndex = 0; }

        /*
        * @brief Records generation, then temporal and spatial resampling as the options ask.
        * Compute subgraph, after the G-buffer pass and outside any render pass.
        * @details Advances the sample index. `gbufferParity` selects which of the two table sets
        * the passes bind: the G-buffer's current/previous roles swap on every advanceFrame(),
        * while a resource table is immutable, so the pass keeps one table per role assignment
        * and the caller says which is live -- the number of advanceFrame() calls since this
        * pass's initialize, modulo 2. A caller that never flips passes 0.
        */
        void recordResample(VkmCommandBufferBase* commandBuffer, const VkmRestirOptions& options,
                            uint32_t gbufferParity = 0);

        /*
        * @brief Records the compute resolve: shades the resampled reservoirs into the
        * accumulation buffer (rgb summed, a = sample count). Same subgraph rules as
        * recordResample, and must follow it in the frame.
        */
        void recordResolveAccumulate(VkmCommandBufferBase* commandBuffer, const VkmRestirOptions& options,
                                     uint32_t gbufferParity = 0);

        // recordResample then recordResolveAccumulate: the accumulating shape the MSE gates use.
        void recordAccumulate(VkmCommandBufferBase* commandBuffer, const VkmRestirOptions& options);

        /*
        * @brief Stages this frame's lighting constants into the pass-owned uniform buffer.
        * Transfer subgraph, before recordResample in the same frame -- the slice indices it
        * writes are the ones that resample will produce.
        * @param misBlend 0 disables 8.7's roughness blend; 1 fully enables it.
        */
        void recordUpdateLightingConstants(VkmCommandBufferBase* commandBuffer, uint32_t frameIndex,
                                           const VkmRestirOptions& options,
                                           VkmRestirDebugView debugView, float misBlend);

        /*
        * @brief Draws the fullscreen lighting triangle from the resampled reservoirs. Graphics
        * subgraph whose color attachment is the indirect-radiance target; writes incoming
        * irradiance, with albedo and 1/pi left to the composite.
        */
        void recordLighting(VkmCommandBufferBase* commandBuffer, uint32_t gbufferParity = 0);

        /*
        * @brief Slice the next recordResample will leave the frame's result in, given `options`.
        * @details Valid until recordResample runs (it advances the sample index the routing is
        * derived from).
        */
        uint32_t getPlannedOutputSlice(const VkmRestirOptions& options) const;

        inline VkmResourceHandle getAccumulationBuffer() const { return _accumulationBuffer; }
        inline VkmResourceHandle getReservoirBuffer() const { return _reservoirBuffer; }
        // For the caller's subgraph declarations around recordUpdateLightingConstants.
        inline VkmResourceHandle getLightingConstantBuffer() const { return _lightingConstantBuffer; }
        inline VkmResourceHandle getLightingStagingBuffer(uint32_t frameIndex) const
        {
            return _lightingStaging[frameIndex];
        }
        inline uint32_t getSampleCount() const { return _sampleIndex; }
        inline uint32_t getWidth() const { return _width; }
        inline uint32_t getHeight() const { return _height; }

    private:
        uint32_t _width = 0;
        uint32_t _height = 0;
        uint32_t _sampleIndex = 0;
        uint32_t _lastResolvedSlice = 0;

        VkmResourceHandle _reservoirBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _neighbourOffsetBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _accumulationBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _lightingConstantBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _lightingStaging[FRAME_BUFFER_COUNT]{};
        VkmStagingBuffer* _lightingStagingPointers[FRAME_BUFFER_COUNT]{};
        VkmPipelineStateBase* _generatePipeline = nullptr;
        VkmPipelineStateBase* _temporalPipeline = nullptr;
        VkmPipelineStateBase* _spatialPipeline = nullptr;
        VkmPipelineStateBase* _resolvePipeline = nullptr;
        VkmPipelineStateBase* _lightingPipeline = nullptr;
        // [parity]: which of the G-buffer's two texture sets currently plays "current". Index 0
        // is the assignment at initialize; index 1 is the same bindings with the sets swapped.
        VkmResourceTableBase* _generateTables[2] = { nullptr, nullptr };
        VkmResourceTableBase* _temporalTables[2] = { nullptr, nullptr };
        VkmResourceTableBase* _spatialTables[2] = { nullptr, nullptr };
        VkmResourceTableBase* _resolveTables[2] = { nullptr, nullptr };
        VkmResourceTableBase* _lightingTables[2] = { nullptr, nullptr };
    };
} // namespace vkm
