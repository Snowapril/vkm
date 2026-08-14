// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#include <glm/vec2.hpp>

#include <cstdint>

namespace vkm
{
    class VkmDriverBase;
    class VkmRenderGraph;
    class VkmCommandBufferBase;

    /*
    * @brief Radical-inverse of `index` in the given base, the classic low-discrepancy sequence.
    * @param index One-based sequence position; index 0 returns 0.
    * @param base Radix, at least 2. Bases 2 and 3 form the standard 2D Halton pair.
    * @return A value in [0, 1).
    */
    float vkmHalton(uint32_t index, uint32_t base);

    /*
    * @brief Number of jitter samples to cycle before repeating, for a given upscale ratio.
    * @details ceil(8 * (displayWidth / renderWidth)^2) -- the sequence length both MetalFX and
    * FSR recommend, long enough that every display pixel is eventually sampled.
    * @param renderWidth Render-resolution width in pixels; must be non-zero.
    * @param displayWidth Display-resolution width in pixels.
    */
    uint32_t vkmUpscaleJitterPhaseCount(uint32_t renderWidth, uint32_t displayWidth);

    /*
    * @brief Sub-pixel jitter for one frame, as a Halton(2,3) offset from the pixel center.
    * @details Cycles through `phaseCount` samples; feed the result to both
    * VkmCamera::setJitterPixels and VkmUpscalerDispatchDesc::_jitterPixels.
    * @param frameCounter Monotonic frame counter.
    * @param phaseCount Sequence length, from vkmUpscaleJitterPhaseCount; must be non-zero.
    * @return An offset in pixels, each component in [-0.5, 0.5).
    */
    glm::vec2 vkmUpscaleJitterPixels(uint64_t frameCounter, uint32_t phaseCount);

    /*
    * @brief Immutable description of a temporal upscaler, fixed at creation.
    * @details Extents are fixed for the upscaler's lifetime: a resize destroys and recreates it
    * (retired only once no in-flight frame still references it, like a resource table). Formats
    * name the textures later passed through VkmUpscalerDispatchDesc; the motion texture may carry
    * unrelated data outside .xy, which is all the upscaler reads.
    */
    struct VkmUpscalerDescriptor
    {
        glm::uvec2 _renderExtent{ 0, 0 };
        glm::uvec2 _displayExtent{ 0, 0 };
        VkmFormat _colorFormat = VkmFormat::R16G16B16A16_SFLOAT;  // linear HDR, pre-tonemap
        VkmFormat _depthFormat = VkmFormat::D32_SFLOAT;
        VkmFormat _motionFormat = VkmFormat::R16G16B16A16_SFLOAT; // xy = UV-space motion
        VkmFormat _outputFormat = VkmFormat::R16G16B16A16_SFLOAT;
        bool _depthInverted = false; // false: near maps to depth 0 (glm::perspectiveRH_ZO)
        bool _autoExposure = true;
        const char* _debugName = nullptr;
    };

    /*
    * @brief Per-frame inputs for one upscale dispatch.
    * @details All textures are render-extent sized except _output, which is display-extent sized
    * and must allow shader writes. _motion carries UV-space current-to-previous motion in .xy,
    * the convention vkmComputeMotionVector writes. _jitterPixels must equal the value the camera
    * was jittered by this frame (VkmCamera::setJitterPixels).
    */
    struct VkmUpscalerDispatchDesc
    {
        VkmResourceHandle _color{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _depth{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _motion{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _output{ VKM_INVALID_RESOURCE_HANDLE };
        glm::vec2 _jitterPixels{ 0.0f, 0.0f };
        float _deltaTimeSeconds = 0.0f;
        bool _reset = false; // discard history: first frame, resize, camera cut
        // Camera parameters some upscalers need for depth linearization; Metal ignores them.
        float _nearZ = 0.0f;
        float _farZ = 0.0f;
        float _fovYRadians = 0.0f;
    };

    /*
    * @brief Backend-agnostic temporal upscaler: render-extent inputs in, display-extent image out.
    * @details Created through VkmDriverBase::newUpscaler, which returns null on a backend whose
    * capability flags lack VkmDriverCapabilityFlags::TemporalUpscaling. The caller owns the
    * result: destroy() then delete, once no in-flight frame can still be using it (the same
    * lifetime contract as VkmResourceTableBase). The upscaler keeps internal history between
    * dispatches; pass _reset when that history no longer matches the scene.
    */
    class VkmUpscalerBase
    {
    public:
        virtual ~VkmUpscalerBase() = default;

        /*
        * @brief Creates the backend upscaler for a fixed render/display extent pair.
        * @param driver Driver this upscaler records through; borrowed, must outlive the upscaler.
        * @param descriptor Extents and formats; both extents must be non-zero and the render
        * extent must not exceed the display extent.
        * @return False with an error logged if the backend rejected the description.
        */
        bool initialize(VkmDriverBase* driver, const VkmUpscalerDescriptor& descriptor);

        void destroy();

        /*
        * @brief Appends the upscale pass to `renderGraph` as one compute subgraph.
        * @details Declares color/depth/motion as ShaderSampledRead and the output as
        * ShaderStorageReadWrite, so the graph's barrier plan orders the pass like any other
        * consumer -- the backend encode itself happens inside the subgraph's callback. Insert it
        * after the pass writing _color and before whatever samples _output.
        * @param renderGraph Graph for the current frame.
        * @param dispatchDesc This frame's inputs; handles must be valid.
        */
        void recordDispatch(VkmRenderGraph* renderGraph, const VkmUpscalerDispatchDesc& dispatchDesc);

        inline const VkmUpscalerDescriptor& getDescriptor() const { return _descriptor; }

    protected:
        virtual bool initializeInner() = 0;
        virtual void destroyInner() = 0;

        /*
        * @brief Records the backend upscale into `commandBuffer`.
        * @details Called from the compute subgraph's callback with no render pass open. The
        * declared barriers have already run: inputs are readable, the output is writable.
        */
        virtual void encodeInner(VkmCommandBufferBase* commandBuffer,
                                 const VkmUpscalerDispatchDesc& dispatchDesc) = 0;

        VkmDriverBase* _driver = nullptr;
        VkmUpscalerDescriptor _descriptor{};
    };
} // namespace vkm
