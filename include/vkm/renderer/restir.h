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
    class VkmGBuffer;
    class VkmPipelineStateBase;
    class VkmPipelineStateManager;
    class VkmResourceTableBase;

    // Mirrors VKM_RESERVOIR_WORD_STRIDE in vkm_reservoir.hlsli. TestReservoirLayout pins them.
    inline constexpr uint32_t kVkmReservoirWordStride = 8;
    inline constexpr uint32_t kVkmReservoirByteStride = kVkmReservoirWordStride * 4;

    /*
    * @brief How many screen-sized reservoir slices the buffer holds.
    *
    * Two, because that is what resampling needs: a pass reads one slice and writes another, and
    * making that a slice index rather than a second buffer is what keeps a bypass or validation
    * mode a change of index (restir.md section 8.1).
    *
    * **Deliberately not FRAME_COUNT.** With `FRAME_COUNT = 3`, frames N-1 and N-2 may still be
    * executing when N is recorded, so a naive per-frame slice would need three. It does not,
    * because `VkmRenderGraph` already calls `ensureCompleted()` on a frame slot before recording
    * into it again: by the time frame N records, the frame that last wrote these slices has been
    * waited on. That is the same guarantee descriptor set 1's per-slot region and the
    * push-constant ring both rely on, and it is why the reservoir buffer is sized by what the
    * resampling passes need rather than by how many frames are in flight.
    */
    inline constexpr uint32_t kVkmReservoirSliceCount = 2;

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
        uint32_t _pad0 = 0;
    };
    static_assert(sizeof(VkmRestirConstants) == 40, "VkmRestirConstants must match the shader-side struct");

    struct VkmRestirOptions
    {
        // Scatters from the G-buffer surface, matching VkmIndirectOptions: a reference run needs
        // one more, because its first bounce is the primary ray neither pass casts.
        uint32_t _maxBounces = 3;
        glm::vec3 _environmentRadiance{ 0.0f, 0.0f, 0.0f };
        // Which slice generation writes and resolve reads. Equal today; 8.4's spatial pass is what
        // makes them differ.
        uint32_t _outputSlice = 0;
        uint32_t _inputSlice = 0;
    };

    /*
    * @brief Phase 8's ReSTIR GI passes. Currently sub-steps 8.1, 8.3 and the resolve half of 8.6:
    * a reservoir buffer, one traced sample per pixel written into it, and shading from it.
    *
    * @details **No resampling yet, and that is what makes this step checkable.** With a single
    * candidate, RIS reduces to the estimator it resamples -- `W = 1/p_source` -- so resolving a
    * freshly generated reservoir must reproduce `VkmIndirectPass` sample for sample. The only
    * thing that can separate them is the reservoir round trip, which is exactly what this
    * sub-step exists to validate before spatial and temporal reuse make a bias hard to attribute.
    *
    * Its accumulation buffer has the same layout `VkmPathTracer` and `VkmIndirectPass` use (rgb
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
        * @brief Records generation then resolve, one sample per pixel. Compute subgraph, after the
        * G-buffer pass and outside any render pass.
        *
        * One entry point rather than two, because nothing yet goes between them; 8.4 and 8.5 are
        * what split it.
        */
        void recordAccumulate(VkmCommandBufferBase* commandBuffer, const VkmRestirOptions& options);

        inline VkmResourceHandle getAccumulationBuffer() const { return _accumulationBuffer; }
        inline VkmResourceHandle getReservoirBuffer() const { return _reservoirBuffer; }
        inline uint32_t getSampleCount() const { return _sampleIndex; }
        inline uint32_t getWidth() const { return _width; }
        inline uint32_t getHeight() const { return _height; }

    private:
        uint32_t _width = 0;
        uint32_t _height = 0;
        uint32_t _sampleIndex = 0;

        VkmResourceHandle _reservoirBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _accumulationBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmPipelineStateBase* _generatePipeline = nullptr;
        VkmPipelineStateBase* _resolvePipeline = nullptr;
        VkmResourceTableBase* _generateTable = nullptr;
        VkmResourceTableBase* _resolveTable = nullptr;
    };
} // namespace vkm
