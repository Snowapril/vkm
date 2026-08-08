// Copyright (c) 2025 Snowapril

#pragma once

#include <glm/vec4.hpp>

#include <cstdint>

namespace vkm
{
    /*
    * @brief What the shared GI composite writes.
    *
    * @details The composite is the one pass that knows how a technique's output is combined, so it
    * is also the engine's view into the G-buffer: these are the channels the deferred path
    * produces, selectable here rather than in every sample's own visualiser.
    * Must match the VKM_GI_DEBUG_* defines in shaders/gi_composite.hlsl.
    */
    enum class VkmGiDebugView : uint32_t
    {
        Composite = 0,      // direct + indirect, the real image
        Direct = 1,
        Indirect = 2,       // the technique's output alone, before albedo
        Albedo = 3,
        Normal = 4,         // shading normal, remapped to [0,1]
        GeometricNormal = 5,
        Roughness = 6,
        Metallic = 7,
        Motion = 8,         // screen-space motion, scaled to be visible
        CameraDistance = 9, // reciprocal, so near geometry is bright
        Count = 10,
    };

    // Display names in VkmGiDebugView order, for a UI that offers the views.
    const char* vkmGiDebugViewName(VkmGiDebugView view);

    /*
    * @brief The composite's per-frame settings.
    *
    * Mirrors GiCompositeConstants in shaders/gi_composite.hlsl. One vec4 because a fragment shader
    * cannot read push constants on Vulkan, so this rides a set-2 uniform buffer whose contents are
    * rewritten per frame -- the table binding it stays immutable, as per-pass tables must.
    */
    struct VkmGiCompositeConstants
    {
        // x = indirect intensity, y = debug view, zw unused
        glm::vec4 _params{ 1.0f, 0.0f, 0.0f, 0.0f };
    };
    static_assert(sizeof(VkmGiCompositeConstants) == 16,
                  "VkmGiCompositeConstants must match GiCompositeConstants in gi_composite.hlsl");

    /*
    * @brief The screen-space contact term's parameters.
    *
    * @details Rays are short: this pass exists for what the probe grid is too coarse to hold --
    * contact darkening, near-field bounce -- and everything beyond that range is the probe volume's
    * job. Lengthening them makes it a screen-space GI solution, with the view dependence that
    * implies, not a general one.
    * Mirrors SsgiConstants in shaders/ssgi.hlsl.
    */
    struct VkmSsgiConstants
    {
        // x = ray count, y = world-space ray length, z = steps per ray, w = intensity
        glm::vec4 _params{ 4.0f, 0.5f, 8.0f, 1.0f };
    };
    static_assert(sizeof(VkmSsgiConstants) == 16,
                  "VkmSsgiConstants must match SsgiConstants in ssgi.hlsl");
} // namespace vkm
