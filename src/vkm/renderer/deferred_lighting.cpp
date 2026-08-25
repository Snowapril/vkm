// Copyright (c) 2026 Snowapril

#include <vkm/renderer/deferred_lighting.h>

#include <vkm/renderer/scene/scene.h>

#include <algorithm>
#include <cstring>

namespace vkm
{
    void vkmBuildDeferredLightConstants(const VkmScene& scene, VkmDeferredLightConstants* outConstants)
    {
        VKM_ASSERT(outConstants != nullptr, "vkmBuildDeferredLightConstants needs an output");

        *outConstants = VkmDeferredLightConstants{};

        uint32_t count = 0;
        const auto append = [&](const VkmPunctualLight& light) {
            if (count < kVkmMaxPunctualLights)
            {
                outConstants->_lights[count] = light;
                ++count;
            }
        };

        // The scene's own directional light first, so a scene that overflows the cap keeps the
        // one light every scene has rather than losing it to whichever model imported the most.
        const glm::vec3& sunRadiance = scene.getDirectionalRadiance();
        if (sunRadiance.x > 0.0f || sunRadiance.y > 0.0f || sunRadiance.z > 0.0f)
        {
            VkmPunctualLight sun;
            sun._type = static_cast<uint32_t>(VkmLightType::Directional);
            // The scene stores the direction TOWARDS the light; a light record stores the
            // direction it points, which is the opposite.
            const glm::vec3 aim = -scene.getDirectionalDirection();
            sun._directionWorld[0] = aim.x;
            sun._directionWorld[1] = aim.y;
            sun._directionWorld[2] = aim.z;
            sun._radiance[0] = sunRadiance.x;
            sun._radiance[1] = sunRadiance.y;
            sun._radiance[2] = sunRadiance.z;
            append(sun);
        }

        for (const VkmPunctualLight& light : scene.getPunctualLights())
        {
            append(light);
        }

        outConstants->_lightCount = glm::uvec4(count, 0u, 0u, 0u);
    }

    void vkmBuildDeferredLightConstants(const std::vector<VkmPunctualLight>& lights,
                                        uint32_t tilesPerRow, uint32_t tileSize,
                                        VkmDeferredLightConstants* outConstants)
    {
        VKM_ASSERT(outConstants != nullptr, "vkmBuildDeferredLightConstants needs an output");

        *outConstants = VkmDeferredLightConstants{};
        const uint32_t count = std::min(static_cast<uint32_t>(lights.size()), kVkmMaxPunctualLights);
        for (uint32_t i = 0; i < count; ++i)
        {
            outConstants->_lights[i] = lights[i];
        }
        outConstants->_lightCount = glm::uvec4(count, tilesPerRow, tileSize, 0u);
    }
} // namespace vkm
