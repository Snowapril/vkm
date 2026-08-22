// Copyright (c) 2026 Snowapril

#include <vkm/renderer/shadow_atlas.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/probe_volume.h>
#include <vkm/renderer/scene/scene.h>

#include <glm/common.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace vkm
{
    namespace
    {
        bool fail(std::string* outError, const char* message)
        {
            if (outError != nullptr)
            {
                *outError = message;
            }
            VKM_DEBUG_ERROR(message);
            return false;
        }

        // Tiles laid out in the squarest grid that holds them all, so the atlas stays close to
        // square whatever kVkmMaxShadowTiles is set to.
        uint32_t tilesPerRowFor(uint32_t tileCount)
        {
            uint32_t perRow = 1u;
            while (perRow * perRow < tileCount)
            {
                ++perRow;
            }
            return perRow;
        }
    } // namespace

    VkmShadowAtlas::~VkmShadowAtlas()
    {
        destroy();
    }

    uint32_t VkmShadowAtlas::getTilesPerRow() const
    {
        return tilesPerRowFor(kVkmMaxShadowTiles);
    }

    glm::uvec2 VkmShadowAtlas::getAtlasExtent() const
    {
        const uint32_t perRow = getTilesPerRow();
        const uint32_t rows = (kVkmMaxShadowTiles + perRow - 1u) / perRow;
        return glm::uvec2(perRow * _descriptor._tileSize, rows * _descriptor._tileSize);
    }

    bool VkmShadowAtlas::initialize(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager,
                                    const Descriptor& descriptor, std::string* outError)
    {
        VKM_ASSERT(driver != nullptr, "VkmShadowAtlas needs a driver");
        VKM_ASSERT(pipelineStateManager != nullptr, "VkmShadowAtlas needs a pipeline state manager");

        if (descriptor._tileSize == 0u)
        {
            return fail(outError, "A shadow atlas needs a non-zero tile size");
        }
        if (descriptor._cullViewIndex >= kVkmSceneMaxCullViews)
        {
            return fail(outError, "A shadow atlas's cull view index is out of range");
        }

        _driver = driver;
        _descriptor = descriptor;

        if (!createTargets(outError) || !createConstantBuffer(outError) ||
            !createTable(pipelineStateManager, outError))
        {
            destroy();
            return false;
        }
        return true;
    }

    bool VkmShadowAtlas::createTargets(std::string* outError)
    {
        const glm::uvec2 extent = getAtlasExtent();

        VkmTextureInfo colorInfo{};
        colorInfo._flags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowColorAttachment) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferSrc));
        colorInfo._extent = glm::uvec3(extent, 1);
        colorInfo._numMipLevels = 1;
        colorInfo._numArrayLayers = 1;
        // RGBA16F rather than a single-channel float: the engine's format list has no
        // single-channel format, and the 32-bit ones are non-filterable in core WebGPU, which
        // the per-pass bind-group layout cannot express. This is the format probe capture
        // already renders distance into on all three backends.
        colorInfo._format = VkmFormat::R16G16B16A16_SFLOAT;
        colorInfo._debugName = "VkmShadowAtlasColor";

        VkmTexture* color = _driver->newTexture(colorInfo);
        if (color == nullptr)
        {
            return fail(outError, "Failed to create the shadow atlas colour target");
        }
        _atlasColor = color->getHandle();

        VkmTextureInfo depthInfo{};
        depthInfo._flags = VkmResourceCreateInfo::AllowDepthStencilAttachment;
        depthInfo._extent = glm::uvec3(extent, 1);
        depthInfo._numMipLevels = 1;
        depthInfo._numArrayLayers = 1;
        depthInfo._format = VkmFormat::D32_SFLOAT;
        depthInfo._debugName = "VkmShadowAtlasDepth";

        VkmTexture* depth = _driver->newTexture(depthInfo);
        if (depth == nullptr)
        {
            return fail(outError, "Failed to create the shadow atlas depth target");
        }
        _atlasDepth = depth->getHandle();
        return true;
    }

    bool VkmShadowAtlas::createConstantBuffer(std::string* outError)
    {
        VkmBufferInfo info{};
        info._flags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst));
        info._size = sizeof(VkmShadowAtlasConstants);
        info._debugName = "VkmShadowAtlasConstants";

        VkmBuffer* buffer = _driver->newBuffer(info);
        if (buffer == nullptr)
        {
            return fail(outError, "Failed to create the shadow atlas constant buffer");
        }
        _constantBuffer = buffer->getHandle();

        // One staging region per frame slot: the directional tile is refitted every frame now,
        // so the constants change while the tables naming them stay immutable. Copying inside
        // the frame's own transfer subgraph is what keeps that safe -- an uploadToBuffer here
        // would submit and wait while earlier frames are still sampling the buffer.
        for (uint32_t slot = 0; slot < FRAME_BUFFER_COUNT; ++slot)
        {
            VkmStagingBufferInfo stagingInfo{};
            stagingInfo._size = sizeof(VkmShadowAtlasConstants);
            stagingInfo._debugName = "VkmShadowAtlasConstantStaging";
            VkmStagingBuffer* staging = _driver->newStagingBuffer(stagingInfo);
            if (staging == nullptr)
            {
                return fail(outError, "Failed to create the shadow atlas constant staging buffer");
            }
            _constantStaging[slot] = staging->getHandle();
            _constantStagingPointers[slot] = staging;
        }

        // Uploaded once here so the buffer is never read uninitialized; allocate() rewrites it.
        return _driver->uploadToBuffer(_constantBuffer, &_constants, sizeof(_constants));
    }

    bool VkmShadowAtlas::createTable(VkmPipelineStateManager* pipelineStateManager, std::string* outError)
    {
        // One pipeline per vertex layout, as every scene-drawing pass has: a scene's batches are
        // grouped by layout and each permutation is a separate pipeline.
        for (uint32_t preset = 0; preset < static_cast<uint32_t>(VkmVertexLayoutPreset::Count); ++preset)
        {
            const std::string name = std::string("shadow_depth_pso[") +
                                     vkmVertexLayoutPresetName(static_cast<VkmVertexLayoutPreset>(preset)) + "]";
            VkmPipelineStateBase* pipeline =
                pipelineStateManager->getPipelineState(name, VkmPipelineStateOrigin::Engine);
            if (pipeline == nullptr)
            {
                return fail(outError, "A shadow_depth_pso vertex-layout permutation is missing");
            }
            _pipelines[preset] = pipeline;
            _tables[preset] =
                _driver->newResourceTable(pipeline, VkmResourceSetKind::PerPass, {{ 0, _constantBuffer }}, outError);
            if (_tables[preset] == nullptr)
            {
                return false;
            }
        }
        return true;
    }

    bool VkmShadowAtlas::prepareScene(const VkmScene& scene, std::string* outError)
    {
        for (uint32_t preset = 0; preset < static_cast<uint32_t>(VkmVertexLayoutPreset::Count); ++preset)
        {
            if (_pipelines[preset] != nullptr &&
                !_materialTables[preset].initialize(_driver, scene, _pipelines[preset], outError))
            {
                return false;
            }
        }
        return true;
    }

    void VkmShadowAtlas::appendTile(const glm::mat4& viewProjection, const Tile& tile)
    {
        const uint32_t index = static_cast<uint32_t>(_tiles.size());
        _constants._tileViewProjection[index] = viewProjection;
        _constants._tileLightPosition[index] =
            glm::vec4(tile._lightPosition, tile._positional ? 1.0f : 0.0f);
        _constants._tileLightDirection[index] = glm::vec4(tile._lightDirection, tile._texelWorldPerDistance);
        _tiles.push_back(tile);
    }

    void VkmShadowAtlas::allocate(const VkmScene& scene, std::vector<VkmPunctualLight>* outLights)
    {
        VKM_ASSERT(isValid(), "VkmShadowAtlas::allocate requires an initialized atlas");
        VKM_ASSERT(outLights != nullptr, "VkmShadowAtlas::allocate needs a light list");

        _tiles.clear();
        _constants = VkmShadowAtlasConstants{};

        // The pass pushes once per (tile, draw batch), plus once per batch for its own cull, so
        // its share of the shared ring is what caps the tiles a frame can fill. Clamped and
        // logged rather than silently overflowing, which reuses live entries.
        const uint32_t batchCount = static_cast<uint32_t>(scene.getDrawBatches().size());
        const uint32_t ringTiles =
            batchCount > 0u ? (kVkmShadowPushConstantReserve / batchCount) : kVkmMaxShadowTiles;
        const uint32_t tileBudget = std::clamp(ringTiles, 1u, kVkmMaxShadowTiles);
        if (tileBudget < kVkmMaxShadowTiles)
        {
            VKM_DEBUG_INFO(("Shadow tiles limited to " + std::to_string(tileBudget) + " (from " +
                            std::to_string(kVkmMaxShadowTiles) + ") by the push-constant ring at " +
                            std::to_string(batchCount) + " draw batches").c_str());
        }

        const VkmSceneAABB bounds = scene.computeWorldBounds();
        const glm::vec3 sceneMin = bounds._valid ? bounds._min : glm::vec3(-1.0f);
        const glm::vec3 sceneMax = bounds._valid ? bounds._max : glm::vec3(1.0f);
        const glm::vec3 sceneCenter = 0.5f * (sceneMin + sceneMax);
        const float sceneRadius = std::max(0.5f * glm::length(sceneMax - sceneMin), 1e-3f);
        _sceneCenter = sceneCenter;
        _sceneRadius = sceneRadius;
        _directionalTile = -1;

        _casterBoxMin = glm::vec3(std::numeric_limits<float>::max());
        _casterBoxMax = glm::vec3(std::numeric_limits<float>::lowest());

        for (VkmPunctualLight& light : *outLights)
        {
            light._shadowTile = -1;

            const glm::vec3 position(light._positionWorld[0], light._positionWorld[1], light._positionWorld[2]);
            const glm::vec3 aim(light._directionWorld[0], light._directionWorld[1], light._directionWorld[2]);
            const float aimLength = glm::length(aim);
            const glm::vec3 direction = aimLength > 0.0f ? aim / aimLength : glm::vec3(0.0f, 0.0f, -1.0f);

            const uint32_t type = light._type;
            const uint32_t cascades =
                std::clamp(_descriptor._cascadeCount, 1u, kVkmMaxShadowCascades);
            const uint32_t needed = (type == static_cast<uint32_t>(VkmLightType::Point)) ? 6u
                                    : (type == static_cast<uint32_t>(VkmLightType::Directional))
                                        ? cascades
                                        : 1u;
            if (_tiles.size() + needed > tileBudget)
            {
                // Out of tiles: the light still shades, it just casts no shadow. A dropped light
                // would be a hole in the image; an unshadowed one is only a missing shadow.
                continue;
            }

            light._shadowTile = static_cast<int32_t>(_tiles.size());
            light._shadowTileCount = needed;

            if (type == static_cast<uint32_t>(VkmLightType::Directional))
            {
                // Only one directional tile is tracked for refitting; a second directional light
                // keeps whatever fit allocate() gave it.
                if (_directionalTile < 0)
                {
                    _directionalTile = static_cast<int32_t>(_tiles.size());
                    _directionalCascades = cascades;
                    _directionalDirection = direction;
                }
                for (uint32_t cascade = 0; cascade < cascades; ++cascade)
                {
                    Tile tile;
                    tile._lightDirection = direction;
                    tile._positional = false;
                    appendTile(glm::mat4(1.0f), tile);
                }
                rebuildDirectionalTile();

                _casterBoxMin = glm::min(_casterBoxMin, sceneMin);
                _casterBoxMax = glm::max(_casterBoxMax, sceneMax);
                continue;
            }

            // A positional light reaches as far as its range, or across the scene when unranged.
            const float farZ = light._range > 0.0f ? light._range : sceneRadius * 4.0f;
            _casterBoxMin = glm::min(_casterBoxMin, position - glm::vec3(farZ));
            _casterBoxMax = glm::max(_casterBoxMax, position + glm::vec3(farZ));

            if (type == static_cast<uint32_t>(VkmLightType::Spot))
            {
                // The cone's full angle, widened a little so the tile's edge texels are never the
                // ones a lookup lands on.
                const float outerAngle = std::acos(std::clamp(light._cosOuter, -1.0f, 1.0f));
                const float fov = std::clamp(2.0f * outerAngle * 1.1f, 0.05f, 3.0f);
                const glm::vec3 up = std::abs(direction.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                                   : glm::vec3(0.0f, 1.0f, 0.0f);
                const glm::mat4 view = glm::lookAtRH(position, position + direction, up);
                const glm::mat4 projection = glm::perspectiveRH_ZO(fov, 1.0f, _descriptor._nearZ, farZ);
                Tile tile;
                tile._lightPosition = position;
                tile._lightDirection = direction;
                tile._positional = true;
                tile._farZ = farZ;
                // Perspective: a texel's footprint grows with distance, and this is its width
                // per unit of it -- the frustum's full angular width over the tile resolution.
                tile._texelWorldPerDistance =
                    2.0f * std::tan(0.5f * fov) / static_cast<float>(_descriptor._tileSize);
                appendTile(projection * view, tile);
                continue;
            }

            // Point: the same six cube-face view-projections a probe capture uses, so the face
            // order the lookup selects by is shared rather than restated.
            glm::mat4 faces[6];
            vkmBuildProbeFaceViewProjections(position, _descriptor._nearZ, farZ, faces);
            for (uint32_t face = 0; face < 6; ++face)
            {
                Tile tile;
                tile._lightPosition = position;
                tile._lightDirection = direction;
                tile._positional = true;
                tile._farZ = farZ;
                // A cube face is a 90-degree frustum, so tan(45 deg) = 1.
                tile._texelWorldPerDistance = 2.0f / static_cast<float>(_descriptor._tileSize);
                appendTile(faces[face], tile);
            }
        }

        if (_tiles.empty())
        {
            _casterBoxMin = sceneMin;
            _casterBoxMax = sceneMax;
        }

        _driver->uploadToBuffer(_constantBuffer, &_constants, sizeof(_constants));
        _constantsDirty = false;
    }

    void VkmShadowAtlas::fitDirectionalCascade(uint32_t tileIndex, const glm::vec3& center, float radius)
    {
        const glm::vec3 direction = _directionalDirection;
        // Any up vector not parallel to the aim; the aim is normalized, so comparing a component
        // against 1 is a safe test for "pointing along Y".
        const glm::vec3 up =
            std::abs(direction.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

        // Snap the centre to whole shadow texels, in light space. Without this the texel grid
        // slides under a static world every time the camera moves and every shadow edge crawls --
        // the fit has to change in texel-sized steps or not at all. Each cascade has its own
        // texel size, so each snaps to its own grid.
        const float texelWorld = (2.0f * radius) / static_cast<float>(_descriptor._tileSize);
        const glm::mat4 snapView = glm::lookAtRH(glm::vec3(0.0f), direction, up);
        glm::vec3 lightSpaceCenter = glm::vec3(snapView * glm::vec4(center, 1.0f));
        lightSpaceCenter.x = std::floor(lightSpaceCenter.x / texelWorld) * texelWorld;
        lightSpaceCenter.y = std::floor(lightSpaceCenter.y / texelWorld) * texelWorld;
        const glm::vec3 snappedCenter =
            glm::vec3(glm::inverse(snapView) * glm::vec4(lightSpaceCenter, 1.0f));

        // Pulled back far enough that casters between the light and the fitted region are still
        // inside the frustum -- an object outside it casts nothing, which reads as a missing
        // shadow rather than as an error.
        const float pullBack = _sceneRadius * 2.0f + radius;
        const glm::vec3 eye = snappedCenter - direction * pullBack;
        const float farZ = pullBack + radius + _sceneRadius * 2.0f;

        const glm::mat4 view = glm::lookAtRH(eye, snappedCenter, up);
        const glm::mat4 projection = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, farZ);

        Tile& tile = _tiles[tileIndex];
        tile._lightPosition = eye;
        tile._lightDirection = direction;
        tile._positional = false;
        tile._farZ = farZ;
        // Orthographic: the footprint does not depend on distance, so it is the whole constant.
        tile._texelWorldPerDistance = texelWorld;

        _constants._tileViewProjection[tileIndex] = projection * view;
        _constants._tileLightPosition[tileIndex] = glm::vec4(tile._lightPosition, 0.0f);
        _constants._tileLightDirection[tileIndex] =
            glm::vec4(tile._lightDirection, tile._texelWorldPerDistance);
    }

    void VkmShadowAtlas::rebuildDirectionalTile()
    {
        if (_directionalTile < 0)
        {
            return;
        }

        for (uint32_t cascade = 0; cascade < _directionalCascades; ++cascade)
        {
            const uint32_t tileIndex = static_cast<uint32_t>(_directionalTile) + cascade;
            if (tileIndex >= _tiles.size())
            {
                break;
            }
            // Without a camera every cascade falls back to the scene fit, which is what an
            // atlas used headlessly gets.
            const glm::vec3 center = _hasFocus ? _cascadeCenters[cascade] : _sceneCenter;
            const float radius = _hasFocus ? _cascadeRadii[cascade] : _sceneRadius;
            fitDirectionalCascade(tileIndex, center, std::max(radius, 1e-3f));
        }
        _constantsDirty = true;
    }

    void VkmShadowAtlas::setDirectionalFocus(const glm::vec3& center, float radius)
    {
        VKM_ASSERT(isValid(), "VkmShadowAtlas::setDirectionalFocus requires an initialized atlas");

        // One sphere for every cascade: a caller that wants a single fitted region rather than a
        // split view still gets one, and the cascades simply coincide.
        _hasFocus = radius > 0.0f;
        _cascadeCenters.fill(center);
        _cascadeRadii.fill(std::max(radius, 0.0f));
        rebuildDirectionalTile();
    }

    void VkmShadowAtlas::setDirectionalView(const glm::vec3& cameraPosition, const glm::vec3& cameraForward,
                                            float fovYRadians, float aspect, float shadowDistance)
    {
        VKM_ASSERT(isValid(), "VkmShadowAtlas::setDirectionalView requires an initialized atlas");

        const float distance = std::max(shadowDistance, 1e-3f);
        const uint32_t cascades = std::max(_directionalCascades, 1u);
        const float lambda = std::clamp(_descriptor._cascadeSplitLambda, 0.0f, 1.0f);
        // A near distance of zero would make the logarithmic term collapse, so the split starts a
        // little way out; anything nearer than this lands in the first cascade anyway.
        const float nearDistance = std::max(distance * 0.005f, 1e-3f);

        const float tanHalfFovY = std::tan(0.5f * std::max(fovYRadians, 1e-3f));
        const glm::vec3 forward =
            glm::length(cameraForward) > 0.0f ? glm::normalize(cameraForward) : glm::vec3(0.0f, 0.0f, -1.0f);

        float sliceNear = nearDistance;
        for (uint32_t cascade = 0; cascade < cascades && cascade < kVkmMaxShadowCascades; ++cascade)
        {
            const float fraction = static_cast<float>(cascade + 1) / static_cast<float>(cascades);
            // The practical split: a blend of the uniform and logarithmic schemes. Uniform alone
            // spends the near cascades on distance the eye barely resolves; logarithmic alone
            // leaves the last cascade covering nearly everything.
            const float uniformSplit = nearDistance + (distance - nearDistance) * fraction;
            const float logSplit = nearDistance * std::pow(distance / nearDistance, fraction);
            const float sliceFar = lambda * logSplit + (1.0f - lambda) * uniformSplit;

            // A sphere bounding the slice's frustum. The lateral term is the far plane's
            // half-diagonal, so the sphere contains the slice whatever the aspect ratio.
            const float halfLength = 0.5f * (sliceFar - sliceNear);
            const float lateral = sliceFar * tanHalfFovY * std::sqrt(1.0f + aspect * aspect);
            _cascadeCenters[cascade] = cameraPosition + forward * (sliceNear + halfLength);
            _cascadeRadii[cascade] = std::sqrt(halfLength * halfLength + lateral * lateral);

            sliceNear = sliceFar;
        }
        _hasFocus = true;
        rebuildDirectionalTile();
    }

    void VkmShadowAtlas::record(VkmRenderGraph* renderGraph, VkmScene* scene,
                                const VkmFrameData& frameData, uint32_t frameIndex)
    {
        VKM_ASSERT(isValid(), "VkmShadowAtlas::record requires an initialized atlas");
        VKM_ASSERT(renderGraph != nullptr && scene != nullptr, "VkmShadowAtlas::record needs a graph and a scene");

        if (_tiles.empty())
        {
            return;
        }

        // Cull against the box every shadow caster can reach, not against a frustum: the pass
        // fills several lights' tiles from one visible list, so no single frustum describes it.
        VkmFrameData shadowFrameData = frameData;
        vkmBuildBoxPlanes(_casterBoxMin, _casterBoxMax, shadowFrameData._frustumPlanes);

        std::vector<VkmResourceAccessDeclaration> sceneResources;
        const auto referenceScene = [&sceneResources, scene](VkmRenderSubGraph* subGraph,
                                                             VkmScene::ReferencePhase phase) {
            sceneResources.clear();
            scene->collectReferencedResources(phase, &sceneResources);
            subGraph->addReferencedResources(sceneResources);
        };

        const uint32_t cullView = _descriptor._cullViewIndex;

        VkmRenderTransferSubGraph* updateSubGraph = renderGraph->beginTransferSubGraph("ShadowSceneUpdate");
        referenceScene(updateSubGraph, VkmScene::ReferencePhase::Update);
        const bool uploadConstants = _constantsDirty;
        if (uploadConstants)
        {
            updateSubGraph->addReferencedResource(_constantBuffer, VkmResourceAccess::TransferWrite);
            updateSubGraph->addReferencedResource(_constantStaging[frameIndex], VkmResourceAccess::TransferRead);
        }
        updateSubGraph->setTransferCallback(
            [this, scene, frameIndex, shadowFrameData, cullView, uploadConstants](VkmCommandBufferBase* commandBuffer) {
                scene->recordUpdate(commandBuffer, frameIndex, shadowFrameData, cullView);
                if (uploadConstants)
                {
                    _constantStagingPointers[frameIndex]->writeDirect(0, &_constants, sizeof(_constants));
                    commandBuffer->copyBuffer(_constantStaging[frameIndex], _constantBuffer, 0, 0,
                                              sizeof(_constants));
                }
            });
        _constantsDirty = false;

        VkmRenderComputeSubGraph* cullSubGraph = renderGraph->beginComputeSubGraph("ShadowSceneCull");
        referenceScene(cullSubGraph, VkmScene::ReferencePhase::Cull);
        cullSubGraph->setComputeCallback([scene, cullView](VkmCommandBufferBase* commandBuffer) {
            scene->recordCull(commandBuffer, cullView);
        });

        const glm::uvec2 extent = getAtlasExtent();
        VkmFrameBufferDescriptor fb{};
        fb._width = extent.x;
        fb._height = extent.y;
        fb._renderPass._colorAttachmentCount = 1;
        fb._renderPass._colorAttachments[0]._attachmentId = 0;
        fb._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::Clear;
        fb._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
        // A far sentinel, not zero: an untouched texel must read as "nothing occludes", and zero
        // would mean "an occluder is exactly at the light".
        fb._renderPass._colorAttachments[0]._clearColors[0] = kVkmShadowFarSentinel;
        fb._renderPass._colorAttachments[0]._clearColors[1] = 0.0f;
        fb._renderPass._colorAttachments[0]._clearColors[2] = 0.0f;
        fb._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;
        fb._colorAttachments[0] = _atlasColor;

        VkmDepthStencilAttachmentDescriptor depthDesc{};
        depthDesc._attachmentId = 1;
        depthDesc._loadAction = VkmLoadAction::Clear;
        depthDesc._storeAction = VkmStoreAction::Store;
        depthDesc._clearDepth = 1.0f;
        fb._renderPass._depthStencilAttachment = depthDesc;
        fb._depthStencilAttachment = _atlasDepth;

        VkmRenderGraphicsSubGraph* subGraph = renderGraph->beginGraphicsSubGraph(fb, "ShadowAtlas");
        referenceScene(subGraph, VkmScene::ReferencePhase::Draw);
        subGraph->addReferencedResource(_atlasColor, VkmResourceAccess::ColorAttachmentWrite);
        subGraph->addReferencedResource(_atlasDepth, VkmResourceAccess::DepthStencilAttachmentWrite);
        for (VkmResourceTableBase* table : _tables)
        {
            if (table != nullptr)
            {
                std::vector<VkmResourceAccessDeclaration> bound;
                table->collectReferencedResources(&bound);
                subGraph->addReferencedResources(bound);
            }
        }

        const uint32_t tileSize = _descriptor._tileSize;
        const uint32_t perRow = getTilesPerRow();
        subGraph->setRenderCallback([this, scene, tileSize, perRow](VkmCommandBufferBase* commandBuffer) {
            for (uint32_t tile = 0; tile < _tiles.size(); ++tile)
            {
                commandBuffer->setViewportAndScissor(static_cast<int32_t>((tile % perRow) * tileSize),
                                                     static_cast<int32_t>((tile / perRow) * tileSize),
                                                     tileSize, tileSize);

                VkmShadowTilePushConstants push{};
                push._tileIndex = tile;
                push._frameDataIndex = _descriptor._cullViewIndex;

                scene->recordDrawBatches(
                    commandBuffer,
                    [this](const VkmScene::DrawBatch& batch) {
                        return _pipelines[static_cast<uint32_t>(batch._layout)];
                    },
                    [this, &push](VkmCommandBufferBase* cb, const VkmScene::DrawBatch& batch) {
                        cb->bindResourceTable(_tables[static_cast<uint32_t>(batch._layout)]);
                        // Set 3 where the backend needs it: the alpha-mask discard samples the
                        // base colour, so a masked leaf must not cast its whole quad's shadow.
                        _materialTables[static_cast<uint32_t>(batch._layout)].bind(cb, batch._materialIndex);
                        cb->setPushConstants(&push, sizeof(push));
                    },
                    _descriptor._cullViewIndex);
            }
        });
    }

    void VkmShadowAtlas::destroy()
    {
        if (_driver == nullptr)
        {
            return;
        }
        for (VkmResourceTableBase*& table : _tables)
        {
            if (table != nullptr)
            {
                table->destroy();
                delete table;
                table = nullptr;
            }
        }
        for (VkmSceneMaterialTables& tables : _materialTables)
        {
            tables.destroy(_driver);
        }
        // Released straight to the pool rather than through the deferred reclaimer, matching
        // VkmProbeVolume::releaseSet: the reclaimer frees on a GPU timeline that will not advance
        // again once teardown has started. Draining before destroy() is the caller's job.
        for (VkmResourceHandle& staging : _constantStaging)
        {
            if (staging.isValid())
            {
                _driver->getRenderResourcePool()->releaseResource(staging);
                staging = VKM_INVALID_RESOURCE_HANDLE;
            }
        }
        _constantStagingPointers.fill(nullptr);
        for (VkmResourceHandle* handle : { &_atlasColor, &_atlasDepth, &_constantBuffer })
        {
            if (handle->isValid())
            {
                _driver->getRenderResourcePool()->releaseResource(*handle);
                *handle = VKM_INVALID_RESOURCE_HANDLE;
            }
        }
        for (VkmPipelineStateBase*& pipeline : _pipelines)
        {
            pipeline = nullptr;
        }
        _tiles.clear();
        _constants = VkmShadowAtlasConstants{};
        _driver = nullptr;
    }
} // namespace vkm
