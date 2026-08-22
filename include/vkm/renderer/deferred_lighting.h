// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/scene/light_table.h>

#include <glm/vec4.hpp>

#include <cstdint>
#include <vector>

namespace vkm
{
    class VkmScene;

    // Matches VKM_MAX_PUNCTUAL_LIGHTS in shaders/vkm_punctual_lights.hlsli.
    constexpr uint32_t kVkmMaxPunctualLights = 16;

    /*
    * @brief The deferred lighting pass's per-pass constants: every light it shades.
    * @details Mirrors LightConstants in shaders/deferred_lighting.hlsl byte for byte.
    *
    * Carried in descriptor set 2 rather than read from the scene's bindless FrameData, because
    * the pass is deliberately scene-free -- that is what lets it run on WebGPU, whose WGSL has no
    * runtime-sized arrays. A fixed cap is the price of a uniform buffer.
    *
    * The directional light is an ordinary entry of type Directional rather than a special case.
    * That is the point: before this existed the sun's radiance was typed twice, once here and
    * once into the scene's light table, with a comment asking the two to stay in sync.
    */
    struct VkmDeferredLightConstants
    {
        // x = valid entries in _lights, y = shadow tiles per atlas row, z = shadow tile size in
        // texels. The atlas geometry rides here rather than in its own buffer because the lookup
        // needs both together and a second uniform binding would buy nothing.
        glm::uvec4 _lightCount{ 0u, 0u, 0u, 0u };
        VkmPunctualLight _lights[kVkmMaxPunctualLights]{};
    };
    static_assert(sizeof(VkmDeferredLightConstants) == 16 + 64 * kVkmMaxPunctualLights,
                  "VkmDeferredLightConstants must match LightConstants in deferred_lighting.hlsl");

    /*
    * @brief Fills the deferred pass's constants from a scene's placed lights.
    * @details The single conversion from "what the scene owns" to "what the pass reads", so the
    * two cannot drift. Lights past kVkmMaxPunctualLights are dropped and the count is clamped;
    * a caller that cares should check getPunctualLights().size() against the cap.
    * @param scene Source of the placed lights. Must have been built.
    * @param outConstants Receives the constants; fully overwritten.
    */
    void vkmBuildDeferredLightConstants(const VkmScene& scene, VkmDeferredLightConstants* outConstants);

    /*
    * @brief The same, for a scene whose lights a shadow atlas has already assigned tiles to.
    * @details Takes the light list `VkmShadowAtlas::allocate` filled in rather than the scene's
    * own, so the `_shadowTile` indices survive into the pass. Call order is therefore: scene
    * lights -> atlas allocate -> this.
    * @param lights The atlas's output list.
    * @param tilesPerRow Atlas tiles per row.
    * @param tileSize Atlas tile size in texels.
    * @param outConstants Receives the constants; fully overwritten.
    */
    void vkmBuildDeferredLightConstants(const std::vector<VkmPunctualLight>& lights,
                                        uint32_t tilesPerRow, uint32_t tileSize,
                                        VkmDeferredLightConstants* outConstants);
} // namespace vkm
