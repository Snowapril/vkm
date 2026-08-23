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

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmStagingBuffer;
    class VkmPipelineStateBase;
    class VkmPipelineStateManager;
    class VkmRenderGraph;
    class VkmResourceTableBase;
    class VkmScene;
    struct VkmFrameData;

    // Matches VKM_MAX_SHADOW_TILES in shaders/shadow_depth.hlsl and vkm_shadow.hlsli. The tiles a
    // frame can hold, not the lights: a point light owns six.
    constexpr uint32_t kVkmMaxShadowTiles = 16;

    // Cascades one directional light may own. Four is the usual ceiling: each is a full scene
    // draw into its own tile, and the resolution a fifth would add is already past what the
    // near cascades resolve.
    constexpr uint32_t kVkmMaxShadowCascades = 4;

    /*
    * @brief Push-constant ring entries reserved for the shadow pass in a frame.
    * @details The ring is one shared budget (kVkmPushConstantRingEntryCount per frame region) and
    * no subsystem owns it, so each one that pushes per draw has to size itself against a share
    * rather than against the whole. This is the shadow pass's: it pushes once per (tile, draw
    * batch) plus once per batch for its cull, so the reserve is what bounds its tile count on a
    * scene with many batches. VkmGiSystem subtracts it before sizing the probe refresh's budget,
    * which is the other per-draw pusher.
    *
    * Sized to carry every tile the atlas can hold on a scene of Sponza's complexity:
    * kVkmMaxShadowTiles across its 25 draw batches is 400 entries. Below that the clamp drops
    * shadow-casting lights rather than reporting a limit the caller could act on.
    */
    constexpr uint32_t kVkmShadowPushConstantReserve = 1024;

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
            /*
            * @brief Cascades the directional light is split into.
            * @details One tile cannot serve a whole view: fitted tightly it runs out before the
            * far geometry, fitted loosely it wastes every texel on distance nothing needs. Each
            * cascade covers a slice of the view and gets a tile of its own, so the near slice --
            * where a texel is worth the most -- gets the same resolution as the far one.
            * Clamped to kVkmMaxShadowCascades, and to 1 for a caller that wants the old
            * single-tile behaviour.
            */
            uint32_t _cascadeCount = 3u;
            /*
            * @brief How the view distance is split between cascades: 0 uniform, 1 logarithmic.
            * @details A uniform split wastes the near cascades on distance the eye barely
            * resolves; a purely logarithmic one crams the far cascades so tightly that the last
            * covers almost everything. The practical scheme blends the two, and 0.6 leans
            * towards logarithmic, which is where perspective actually puts the detail.
            */
            float _cascadeSplitLambda = 0.6f;
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
        * @brief Refits the directional tile around what the camera can actually see.
        * @details A directional light has no position, so its tile has to be fitted to *some*
        * region, and fitting it to the whole scene is what makes the shadow useless on a large
        * one: Sponza is 3721 units across, so a 512-texel tile spends 7.3 world units per texel
        * and every silhouette lands on a staircase several units wide. Fitting to a sphere the
        * camera is looking at spends the same texels on the region being looked at instead.
        *
        * The fit is SNAPPED to whole shadow texels. Without that, refitting each frame slides the
        * texel grid under a static world and every shadow edge crawls -- trading a stationary
        * staircase for a moving one, which is worse.
        *
        * Optional: leave it uncalled and the tile keeps its scene-wide fit.
        * @param center World-space centre of the region to cover.
        * @param radius Its radius, in world units.
        */
        void setDirectionalFocus(const glm::vec3& center, float radius);

        /*
        * @brief Refits every directional cascade around the camera's view.
        * @details Splits [nearDistance, shadowDistance] between the cascades with the practical
        * scheme, bounds each slice's frustum with a sphere, and fits that cascade's tile to it --
        * each snapped to its own texel grid, since each has its own texel size.
        * Beyond shadowDistance nothing casts, which reads as flat lighting rather than a black
        * band: the lookup returns lit when no cascade contains the point.
        * @param cameraPosition World-space eye.
        * @param cameraForward Normalized view direction.
        * @param fovYRadians Vertical field of view.
        * @param aspect Width over height.
        * @param shadowDistance How far from the eye shadows are wanted, in world units.
        */
        void setDirectionalView(const glm::vec3& cameraPosition, const glm::vec3& cameraForward,
                                float fovYRadians, float aspect, float shadowDistance);

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

        void rebuildDirectionalTile();
        // Fits one cascade's tile to a world-space sphere, snapped to that cascade's texel grid.
        void fitDirectionalCascade(uint32_t tileIndex, const glm::vec3& center, float radius);
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
        // The directional light's first tile and how many cascades follow it, so a refit
        // rewrites exactly those.
        int32_t _directionalTile = -1;
        uint32_t _directionalCascades = 0u;
        glm::vec3 _directionalDirection{ 0.0f, -1.0f, 0.0f };
        // The spheres each cascade is fitted to. Empty until a camera says otherwise, in which
        // case every cascade falls back to the scene fit.
        std::array<glm::vec3, kVkmMaxShadowCascades> _cascadeCenters{};
        std::array<float, kVkmMaxShadowCascades> _cascadeRadii{};
        bool _hasFocus = false;
        // The scene fit, kept so a refit can fall back to it and so the caster box stays valid.
        glm::vec3 _sceneCenter{ 0.0f };
        float _sceneRadius = 1.0f;
        bool _constantsDirty = false;

        // Per frame slot, because the constants now change per frame while the tables naming them
        // stay immutable -- the same shape the gi sample's composite constants use.
        std::array<VkmResourceHandle, FRAME_BUFFER_COUNT> _constantStaging{};
        std::array<VkmStagingBuffer*, FRAME_BUFFER_COUNT> _constantStagingPointers{};
        // The union of every shadow caster's influence, which the cull view is tested against.
        glm::vec3 _casterBoxMin{ 0.0f };
        glm::vec3 _casterBoxMax{ 0.0f };
    };
} // namespace vkm
