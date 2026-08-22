// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/scene/light_table.h>
#include <vkm/renderer/scene/scene_material_tables.h>
#include <vkm/renderer/scene/vertex_layout.h>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmPipelineStateBase;
    class VkmPipelineStateManager;
    class VkmRenderGraph;
    class VkmResourceTableBase;
    class VkmScene;
    struct VkmFrameData;

    // Matches VKM_MAX_SHADOW_TILES in shaders/shadow_depth.hlsl and vkm_shadow.hlsli. The tiles a
    // frame can hold, not the lights: a point light owns six.
    constexpr uint32_t kVkmMaxShadowTiles = 16;

    // Matches VKM_SHADOW_FAR_SENTINEL in shaders/vkm_shadow.hlsli: what the atlas clears to, so
    // an untouched texel reads as "nothing occludes" rather than "an occluder at the light".
    // Finite in a half-float, whose largest value is 65504.
    constexpr float kVkmShadowFarSentinel = 60000.0f;

    /*
    * @brief The shadow atlas's parameters as a shader sees them.
    * @details Mirrors ShadowAtlasConstants in shaders/shadow_depth.hlsl byte for byte. One
    * matrix per tile, shared by the pass that fills the atlas and the lookup that reads it, so
    * the two provably agree: there is only one copy.
    */
    struct VkmShadowAtlasConstants
    {
        glm::mat4 _tileViewProjection[kVkmMaxShadowTiles]{};
        // xyz = the light position this tile measures distance from, w = 1 positional / 0
        // directional.
        glm::vec4 _tileLightPosition[kVkmMaxShadowTiles]{};
        // xyz = the direction the light points (read only for a directional tile), w = the world
        // size of one of this tile's texels AT UNIT DISTANCE from the light. A perspective tile
        // scales that by the receiver's distance; an orthographic one does not, which is why the
        // number is stored per tile rather than derived in the shader from a formula that only
        // holds for one of the two.
        glm::vec4 _tileLightDirection[kVkmMaxShadowTiles]{};
    };

    /*
    * @brief The per-tile half of the shadow pass's inputs (push constants, vertex stage).
    * @details Mirrors TilePushConstants in shaders/shadow_depth.hlsl. Only the tile index is
    * per-draw; every matrix rides the constant buffer, exactly as the probe capture splits its
    * face matrices from its per-probe push.
    */
    struct VkmShadowTilePushConstants
    {
        uint32_t _tileIndex = 0u;
        // The cull view this pass published its frame data into. Passed rather than assumed:
        // see the shader's note on why reading view 0 makes the pass depend on the camera's.
        uint32_t _frameDataIndex = 0u;
    };

    /*
    * @brief A tiled shadow map serving every light type from one render pass.
    * @details One RGBA16F colour target holding linear world-space distance from the light in .r,
    * plus a depth target used only for hidden-surface removal. Tiles are assigned per light:
    * one for a directional or spot light, six for a point light's cube faces.
    *
    * An atlas rather than a texture array or a cube map because the engine has no layered
    * rendering at all -- no attachment descriptor carries a slice index, Vulkan pins layerCount
    * to 1, and there is no SV_RenderTargetArrayIndex or geometry stage. `setViewportAndScissor`
    * per tile inside one pass is the only shape available, and it is the one VkmProbeVolumeUpdater
    * already uses for a probe's six faces.
    *
    * Call order: initialize -> prepareScene (after VkmScene::build) -> allocate (whenever the
    * lights change) -> record per frame.
    */
    class VkmShadowAtlas
    {
    public:
        struct Descriptor
        {
            // Side of one square tile, in texels. The atlas is kVkmMaxShadowTiles of these laid
            // out in a square-ish grid.
            uint32_t _tileSize = 512u;
            // The scene cull view this pass owns; the camera keeps 0 and the probe refresh 1.
            uint32_t _cullViewIndex = 2u;
            // Near plane for the perspective tiles. World units, so a scene-scale-relative
            // default is impossible -- the caller knows its scale and this does not.
            float _nearZ = 0.05f;
        };

        VkmShadowAtlas() = default;
        ~VkmShadowAtlas();

        VkmShadowAtlas(const VkmShadowAtlas&) = delete;
        VkmShadowAtlas& operator=(const VkmShadowAtlas&) = delete;

        bool initialize(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager,
                        const Descriptor& descriptor, std::string* outError);
        void destroy();

        /*
        * @brief Builds the per-draw material tables the alpha-mask discard needs.
        * @details Only WebGPU binds set 3; elsewhere this is a no-op. Must follow
        * VkmScene::build(), which is where the material textures are created.
        */
        bool prepareScene(const VkmScene& scene, std::string* outError);

        inline bool isValid() const { return _driver != nullptr; }

        /*
        * @brief Assigns tiles to a scene's lights and builds their view-projections.
        * @details Writes each light's `_shadowTile`, so the caller must build its deferred
        * constants AFTER this. Lights past the tile budget are left unshadowed rather than
        * dropped: an unshadowed light is a light, a dropped one is a hole.
        * @param scene Source of the lights and of the bounds a directional tile is fitted to.
        * @param outLights Receives the lights with their tile assignments filled in.
        */
        void allocate(const VkmScene& scene, std::vector<VkmPunctualLight>* outLights);

        /*
        * @brief Records the atlas fill: scene update, cull, then one graphics pass over all tiles.
        * @details Must precede any pass that reads the atlas. `frameData` is the caller's, copied
        * and re-culled against a box covering every shadow caster.
        */
        void record(VkmRenderGraph* renderGraph, VkmScene* scene, const VkmFrameData& frameData,
                    uint32_t frameIndex);

        inline VkmResourceHandle getAtlasTexture() const { return _atlasColor; }
        inline uint32_t getTileCount() const { return static_cast<uint32_t>(_tiles.size()); }
        inline const Descriptor& getDescriptor() const { return _descriptor; }
        // Texel extent of the whole atlas.
        glm::uvec2 getAtlasExtent() const;
        // Tiles per row in that extent; the lookup needs it to turn a tile index into a rect.
        uint32_t getTilesPerRow() const;
        // The constants the lookup binds, valid after allocate().
        inline const VkmShadowAtlasConstants& getConstants() const { return _constants; }
        inline VkmResourceHandle getConstantBuffer() const { return _constantBuffer; }

    private:
        struct Tile
        {
            glm::vec3 _lightPosition{ 0.0f };
            glm::vec3 _lightDirection{ 0.0f, 0.0f, -1.0f };
            bool _positional = true;
            float _farZ = 1.0f;
            float _texelWorldPerDistance = 0.0f;
        };

        bool createTargets(std::string* outError);
        bool createConstantBuffer(std::string* outError);
        bool createTable(VkmPipelineStateManager* pipelineStateManager, std::string* outError);
        void appendTile(const glm::mat4& viewProjection, const Tile& tile);

        VkmDriverBase* _driver = nullptr;
        Descriptor _descriptor{};

        VkmResourceHandle _atlasColor{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _atlasDepth{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _constantBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceTableBase* _tables[static_cast<size_t>(VkmVertexLayoutPreset::Count)]{};
        VkmPipelineStateBase* _pipelines[static_cast<size_t>(VkmVertexLayoutPreset::Count)]{};
        // Set 3 per material, per vertex-layout permutation; empty where the shader reaches the
        // bindless array instead.
        VkmSceneMaterialTables _materialTables[static_cast<size_t>(VkmVertexLayoutPreset::Count)]{};

        std::vector<Tile> _tiles;
        VkmShadowAtlasConstants _constants{};
        // The union of every shadow caster's influence, which the cull view is tested against.
        glm::vec3 _casterBoxMin{ 0.0f };
        glm::vec3 _casterBoxMax{ 0.0f };
    };
} // namespace vkm
