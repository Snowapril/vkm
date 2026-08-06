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

    // Mirrors IndirectConstants in resources/Shaders/gi_indirect.hlsl. Scalars only, like
    // VkmPathTraceConstants and for the same reason.
    struct VkmIndirectConstants
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
    static_assert(sizeof(VkmIndirectConstants) == 32, "VkmIndirectConstants must match the shader-side struct");

    struct VkmIndirectOptions
    {
        /*
        * Scatters counted from the G-buffer surface, so a reference run needs `_maxBounces + 1` to
        * compute the same quantity: its first bounce is the primary ray this pass does not cast.
        */
        uint32_t _maxBounces = 3;
        // Uniform environment radiance a ray that hits nothing returns, as in VkmPathTraceOptions.
        glm::vec3 _environmentRadiance{ 0.0f, 0.0f, 0.0f };
    };

    /*
    * @brief Phase 7's 1-spp indirect pass: one ray per pixel, no reservoirs, deliberately noisy.
    *
    * @details The baseline ReSTIR has to beat, and the thing that shows the sampling and the BRDF
    * are right before reservoirs make a bias hard to see. It differs from `VkmPathTracer` in its
    * primary hit and nothing else -- taken from the rasterized G-buffer instead of a traced ray --
    * so accumulating it has to converge to what the reference converges to.
    *
    * It computes radiance leaving the G-buffer surface **excluding that surface's own emission**,
    * because the G-buffer has no emissive channel. That is the quantity a deferred GI pass can
    * compute; a scene whose camera-visible surfaces emit is outside what this can reproduce.
    *
    * The accumulation buffer has the same layout `VkmPathTracer`'s does (rgb summed, a = sample
    * count), so `vkmComputeImageMse` compares the two directly. A pixel the G-buffer did not cover
    * is left unaccumulated rather than written black, which is what keeps background out of the
    * comparison.
    */
    class VkmIndirectPass
    {
    public:
        VkmIndirectPass() = default;
        ~VkmIndirectPass() = default;

        VkmIndirectPass(const VkmIndirectPass&) = delete;
        VkmIndirectPass& operator=(const VkmIndirectPass&) = delete;

        /*
        * @brief Creates the accumulation buffer and binds `gbuffer`'s targets. Fails without ray
        * tracing. `gbuffer` must outlive this pass: the resource table names its textures, and it
        * is built once rather than per frame.
        */
        bool initialize(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager,
                        const VkmGBuffer& gbuffer, uint32_t width, uint32_t height,
                        std::string* outError);

        void destroy(VkmDriverBase* driver);

        inline void reset() { _sampleIndex = 0; }

        // Records one ray per pixel. Compute subgraph, after the G-buffer pass that fills its
        // inputs and outside any render pass.
        void recordAccumulate(VkmCommandBufferBase* commandBuffer, const VkmIndirectOptions& options);

        inline VkmResourceHandle getAccumulationBuffer() const { return _accumulationBuffer; }
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
} // namespace vkm
