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
    * @brief Emissive triangles the deferred pass shades as area lights.
    * @details Matches VKM_MAX_AREA_LIGHTS in shaders/vkm_area_lights.hlsli. A separate, larger
    * cap than the punctual one because emitters arrive as triangles: a single quad emitter is
    * already two entries, so a cap of 16 would hold eight quads.
    */
    constexpr uint32_t kVkmMaxAreaLights = 32;

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
        // x = valid entries in _areaLights; yzw unused. Its own vector rather than a fourth
        // component of _lightCount, which is full.
        glm::uvec4 _areaLightCount{ 0u, 0u, 0u, 0u };
        VkmPunctualLight _lights[kVkmMaxPunctualLights]{};
        VkmAreaLight _areaLights[kVkmMaxAreaLights]{};
    };
    static_assert(sizeof(VkmDeferredLightConstants) ==
                      32 + 64 * kVkmMaxPunctualLights + 64 * kVkmMaxAreaLights,
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

    /*
    * @brief Fills the area-light half of the constants from a built scene's emissive triangles.
    * @details Separate from the two builders above because the atlas-aware one takes a light list
    * rather than a scene, and both of them wipe the struct first -- so this must run after
    * whichever one the caller used, and cannot simply be folded into one of them.
    *
    * Triangles past kVkmMaxAreaLights are dropped and the count clamped. They arrive in the light
    * table's own order, which vkmFinalizeLightTable left sorted by nothing in particular: a scene
    * past the cap therefore keeps an arbitrary subset, not the brightest one.
    * @param scene Source of the emissive triangles. Must have been built.
    * @param outConstants Receives the area lights; the punctual half is left untouched.
    */
    void vkmFillDeferredAreaLights(const VkmScene& scene, VkmDeferredLightConstants* outConstants);
} // namespace vkm
