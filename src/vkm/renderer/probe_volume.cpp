// Copyright (c) 2025 Snowapril

#include <vkm/renderer/probe_volume.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/texture.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include <string>

namespace vkm
{
    void vkmBuildProbeFaceViewProjections(const glm::vec3& position, float nearZ, float farZ,
                                          glm::mat4 outFaceViewProjections[6])
    {
        // 90 degrees per face is what tiles a cube exactly. The engine's clip space is +Y up with a
        // [0,1] depth range on every backend, hence the _ZO projection (see camera.h).
        const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, nearZ, farZ);

        // +X, -X, +Y, -Y, +Z, -Z. The up vectors are the standard cubemap ones: the poles use +Z
        // and -Z because +Y/-Y are the view direction there and an up vector may not be parallel
        // to it.
        const glm::vec3 forward[6] = {
            { 1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f},
            { 0.0f,  1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
            { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f},
        };
        const glm::vec3 up[6] = {
            {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
            {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        };

        for (uint32_t face = 0; face < 6; ++face)
        {
            outFaceViewProjections[face] =
                projection * glm::lookAtRH(position, position + forward[face], up[face]);
        }
    }

    VkmFormat VkmProbeVolume::getIrradianceFormat()
    {
        // HDR and signed-capable: indirect radiance is unbounded before tone mapping, and 8-bit
        // would band across the low-frequency gradients this tier exists to produce.
        return VkmFormat::R16G16B16A16_SFLOAT;
    }

    VkmFormat VkmProbeVolume::getDistanceFormat()
    {
        // Only rg are used; see the note in the header about the missing two-channel format.
        return VkmFormat::R16G16B16A16_SFLOAT;
    }

    VkmProbeVolume::~VkmProbeVolume()
    {
        destroy();
    }

    bool VkmProbeVolume::initialize(VkmDriverBase* driver, const Descriptor& descriptor)
    {
        VKM_ASSERT(driver != nullptr, "VkmProbeVolume needs a driver");
        if (descriptor._probeCounts.x == 0 || descriptor._probeCounts.y == 0 || descriptor._probeCounts.z == 0)
        {
            VKM_DEBUG_ERROR("A probe volume needs at least one probe on every axis");
            return false;
        }
        if (descriptor._irradianceResolution == 0 || descriptor._distanceResolution == 0)
        {
            VKM_DEBUG_ERROR("A probe volume needs a non-zero octahedral resolution");
            return false;
        }

        _driver = driver;
        _descriptor = descriptor;
        _currentSet = 0;

        for (uint32_t setIndex = 0; setIndex < _sets.size(); ++setIndex)
        {
            if (!createSet(_sets[setIndex], setIndex))
            {
                destroy();
                return false;
            }
        }
        return true;
    }

    void VkmProbeVolume::destroy()
    {
        if (_driver == nullptr)
        {
            return;
        }
        for (AtlasSet& set : _sets)
        {
            releaseSet(set);
        }
        _driver = nullptr;
        _descriptor = Descriptor{};
        _currentSet = 0;
    }

    void VkmProbeVolume::advanceFrame()
    {
        _currentSet = 1 - _currentSet;
    }

    uint32_t VkmProbeVolume::getProbeCount() const
    {
        return _descriptor._probeCounts.x * _descriptor._probeCounts.y * _descriptor._probeCounts.z;
    }

    glm::uvec3 VkmProbeVolume::getProbeCoord(uint32_t probeIndex) const
    {
        const uint32_t countX = _descriptor._probeCounts.x;
        const uint32_t countY = _descriptor._probeCounts.y;
        return glm::uvec3(probeIndex % countX,
                          (probeIndex / countX) % countY,
                          probeIndex / (countX * countY));
    }

    glm::vec3 VkmProbeVolume::getProbePosition(uint32_t probeIndex) const
    {
        const glm::uvec3 coord = getProbeCoord(probeIndex);
        return _descriptor._origin + glm::vec3(coord) * _descriptor._spacing;
    }

    uint32_t VkmProbeVolume::irradianceCellSize() const
    {
        return _descriptor._irradianceResolution + kBorderTexels * 2u;
    }

    uint32_t VkmProbeVolume::distanceCellSize() const
    {
        return _descriptor._distanceResolution + kBorderTexels * 2u;
    }

    glm::uvec2 VkmProbeVolume::probeCellCoord(uint32_t probeIndex) const
    {
        // X and Y flattened along the atlas width, Z along its height, so one XY slice of the grid
        // is one row of cells.
        const glm::uvec3 coord = getProbeCoord(probeIndex);
        return glm::uvec2(coord.y * _descriptor._probeCounts.x + coord.x, coord.z);
    }

    glm::uvec2 VkmProbeVolume::getIrradianceAtlasExtent() const
    {
        const uint32_t cellsX = _descriptor._probeCounts.x * _descriptor._probeCounts.y;
        return glm::uvec2(cellsX * irradianceCellSize(), _descriptor._probeCounts.z * irradianceCellSize());
    }

    glm::uvec2 VkmProbeVolume::getDistanceAtlasExtent() const
    {
        const uint32_t cellsX = _descriptor._probeCounts.x * _descriptor._probeCounts.y;
        return glm::uvec2(cellsX * distanceCellSize(), _descriptor._probeCounts.z * distanceCellSize());
    }

    glm::uvec2 VkmProbeVolume::getIrradianceProbeTexelOrigin(uint32_t probeIndex) const
    {
        return probeCellCoord(probeIndex) * irradianceCellSize();
    }

    glm::uvec2 VkmProbeVolume::getDistanceProbeTexelOrigin(uint32_t probeIndex) const
    {
        return probeCellCoord(probeIndex) * distanceCellSize();
    }

    VkmResourceHandle VkmProbeVolume::getIrradianceTexture() const { return _sets[_currentSet]._irradiance; }
    VkmResourceHandle VkmProbeVolume::getDistanceTexture() const { return _sets[_currentSet]._distance; }
    VkmResourceHandle VkmProbeVolume::getPrevIrradianceTexture() const { return _sets[1 - _currentSet]._irradiance; }
    VkmResourceHandle VkmProbeVolume::getPrevDistanceTexture() const { return _sets[1 - _currentSet]._distance; }

    VkmProbeVolumeConstants VkmProbeVolume::makeConstants(float normalBias, float hysteresis) const
    {
        VkmProbeVolumeConstants constants{};
        constants._originAndSpacingX =
            glm::vec4(_descriptor._origin, _descriptor._spacing.x);
        constants._spacingYZ = glm::vec4(_descriptor._spacing.y, _descriptor._spacing.z, 0.0f, 0.0f);
        constants._probeCounts = glm::uvec4(_descriptor._probeCounts, 0u);
        constants._atlasParams = glm::vec4(static_cast<float>(_descriptor._irradianceResolution),
                                           static_cast<float>(_descriptor._distanceResolution),
                                           normalBias, hysteresis);
        return constants;
    }

    bool VkmProbeVolume::createSet(AtlasSet& set, uint32_t setIndex)
    {
        const auto createAtlas = [&](const glm::uvec2& extent, VkmFormat format, const char* name,
                                     VkmResourceHandle& outHandle) {
            const std::string debugName = std::string(name) + std::to_string(setIndex);

            VkmTextureInfo info{};
            // Written as an attachment by the update pass, sampled by the lookup, and readable
            // back for the debug views this tier will need to be tuned through.
            info._flags = static_cast<VkmResourceCreateInfo>(
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowColorAttachment) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferSrc) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst));
            info._extent = glm::uvec3(extent, 1);
            info._numMipLevels = 1;
            info._numArrayLayers = 1;
            info._format = format;
            info._debugName = debugName.c_str();

            VkmTexture* texture = _driver->newTexture(info);
            if (texture == nullptr)
            {
                VKM_DEBUG_ERROR("Failed to create a probe atlas");
                return false;
            }
            outHandle = texture->getHandle();
            return true;
        };

        return createAtlas(getIrradianceAtlasExtent(), getIrradianceFormat(), "VkmProbeIrradiance", set._irradiance) &&
               createAtlas(getDistanceAtlasExtent(), getDistanceFormat(), "VkmProbeDistance", set._distance);
    }

    void VkmProbeVolume::releaseSet(AtlasSet& set)
    {
        const auto release = [this](VkmResourceHandle& handle) {
            if (handle.isValid())
            {
                _driver->getRenderResourcePool()->releaseResource(handle);
                handle = VKM_INVALID_RESOURCE_HANDLE;
            }
        };
        release(set._irradiance);
        release(set._distance);
    }
} // namespace vkm
