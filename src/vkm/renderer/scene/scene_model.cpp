// Copyright (c) 2026 Snowapril

#include <vkm/renderer/scene/scene_model.h>

#include <glm/common.hpp>

namespace vkm
{
    void VkmSceneAABB::expand(const glm::vec3& point)
    {
        if (!_valid)
        {
            _min = point;
            _max = point;
            _valid = true;
            return;
        }
        _min = glm::min(_min, point);
        _max = glm::max(_max, point);
    }

    void VkmSceneAABB::expand(const VkmSceneAABB& other)
    {
        if (!other._valid)
        {
            return;
        }
        expand(other._min);
        expand(other._max);
    }

    VkmSceneAABB VkmSceneAABB::transformed(const glm::mat4& transform) const
    {
        VkmSceneAABB result;
        if (!_valid)
        {
            return result;
        }

        // Re-fit around the eight transformed corners; a rotated box no longer stays
        // axis-aligned, so transforming only _min/_max would under-cover it.
        for (uint32_t corner = 0; corner < 8; ++corner)
        {
            const glm::vec3 point{
                (corner & 1u) != 0u ? _max.x : _min.x,
                (corner & 2u) != 0u ? _max.y : _min.y,
                (corner & 4u) != 0u ? _max.z : _min.z,
            };
            result.expand(glm::vec3(transform * glm::vec4(point, 1.0f)));
        }
        return result;
    }

    std::vector<VkmSceneModel::DrawItem> VkmSceneModel::buildDrawList() const
    {
        struct StackEntry
        {
            uint32_t _nodeIndex;
            glm::mat4 _parentTransform;
        };

        std::vector<DrawItem> drawList;
        // Guards against a malformed hierarchy where a node is reachable twice (glTF nodes
        // must form a forest, but nothing in the file format enforces it).
        std::vector<uint8_t> visited(_nodes.size(), 0);

        std::vector<StackEntry> stack;
        stack.reserve(_nodes.size());
        for (auto it = _rootNodeIndices.rbegin(); it != _rootNodeIndices.rend(); ++it)
        {
            stack.push_back(StackEntry{ *it, glm::mat4(1.0f) });
        }

        while (!stack.empty())
        {
            const StackEntry entry = stack.back();
            stack.pop_back();

            if (entry._nodeIndex >= _nodes.size() || visited[entry._nodeIndex] != 0)
            {
                continue;
            }
            visited[entry._nodeIndex] = 1;

            const VkmSceneNode& node = _nodes[entry._nodeIndex];
            const glm::mat4 worldTransform = entry._parentTransform * node._localTransform;

            for (const uint32_t meshIndex : node._meshIndices)
            {
                if (meshIndex < _meshes.size())
                {
                    drawList.push_back(DrawItem{ meshIndex, entry._nodeIndex, worldTransform });
                }
            }

            for (auto it = node._childIndices.rbegin(); it != node._childIndices.rend(); ++it)
            {
                stack.push_back(StackEntry{ *it, worldTransform });
            }
        }

        return drawList;
    }

    std::vector<VkmSceneModel::LightItem> VkmSceneModel::buildLightList() const
    {
        struct StackEntry
        {
            uint32_t _nodeIndex;
            glm::mat4 _parentTransform;
        };

        std::vector<LightItem> lightList;
        // Same guard buildDrawList carries, and for the same reason: nothing in glTF enforces
        // that the nodes form a forest.
        std::vector<uint8_t> visited(_nodes.size(), 0);

        std::vector<StackEntry> stack;
        stack.reserve(_nodes.size());
        for (auto it = _rootNodeIndices.rbegin(); it != _rootNodeIndices.rend(); ++it)
        {
            stack.push_back(StackEntry{ *it, glm::mat4(1.0f) });
        }

        while (!stack.empty())
        {
            const StackEntry entry = stack.back();
            stack.pop_back();

            if (entry._nodeIndex >= _nodes.size() || visited[entry._nodeIndex] != 0)
            {
                continue;
            }
            visited[entry._nodeIndex] = 1;

            const VkmSceneNode& node = _nodes[entry._nodeIndex];
            const glm::mat4 worldTransform = entry._parentTransform * node._localTransform;

            if (node._lightIndex < _lights.size())
            {
                LightItem item;
                item._lightIndex = node._lightIndex;
                item._nodeIndex = entry._nodeIndex;
                item._positionWorld = glm::vec3(worldTransform[3]);
                // glTF aims a light down its node's local -Z. Taking the column rather than
                // inverse-transposing is correct for the direction of an axis (a normal would
                // need the inverse transpose; a transformed axis does not), and it survives the
                // non-uniform scale a node is allowed to carry once normalized.
                const glm::vec3 axis = -glm::vec3(worldTransform[2]);
                const float axisLength = glm::length(axis);
                item._directionWorld =
                    axisLength > 0.0f ? axis / axisLength : glm::vec3(0.0f, 0.0f, -1.0f);
                lightList.push_back(item);
            }

            for (auto it = node._childIndices.rbegin(); it != node._childIndices.rend(); ++it)
            {
                stack.push_back(StackEntry{ *it, worldTransform });
            }
        }

        return lightList;
    }

    VkmSceneAABB VkmSceneModel::computeWorldBounds() const
    {
        VkmSceneAABB bounds;
        for (const DrawItem& item : buildDrawList())
        {
            bounds.expand(_meshes[item._meshIndex]._bounds.transformed(item._worldTransform));
        }
        return bounds;
    }

    uint64_t VkmSceneModel::getTotalVertexCount() const
    {
        uint64_t count = 0;
        for (const VkmSceneMesh& mesh : _meshes)
        {
            count += mesh._vertexCount;
        }
        return count;
    }

    uint64_t VkmSceneModel::getTotalIndexCount() const
    {
        uint64_t count = 0;
        for (const VkmSceneMesh& mesh : _meshes)
        {
            count += mesh._indices.size();
        }
        return count;
    }
} // namespace vkm
