// Copyright (c) 2025 Snowapril

#include <vkm/renderer/imgui/acceleration_structure_inspector.h>
#include <vkm/renderer/acceleration_structure_debug.h>
#include <vkm/renderer/backend/common/acceleration_structure.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/memory_report.h>
#include <vkm/renderer/scene/scene_model.h>

#include <imgui.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkm
{
    namespace
    {
        constexpr float kNodeWidth = 240.0f;
        constexpr float kNodePadding = 8.0f;
        constexpr float kColumnGap = 120.0f;
        constexpr float kNodeGap = 20.0f;
        constexpr float kGraphMargin = 16.0f;
        // Spatial tab: canvas units per world unit, before the canvas's own zoom.
        constexpr float kUnitsPerMeter = 20.0f;

        constexpr ImU32 kSelectionColour = IM_COL32(255, 200, 90, 255);

        // The rows, their collection and the per-instance box build are shared with the 3D
        // overlay; see renderer/acceleration_structure_debug.h.
        using StructureRow = VkmAccelerationStructureSummary;

        uint32_t triangleCount(const VkmAccelerationStructureInfo& info)
        {
            uint32_t triangles = 0;
            for (const VkmAccelerationStructureGeometry& geometry : info._geometries)
            {
                triangles += geometry._indexCount / 3;
            }
            return triangles;
        }

        void drawEmptyHint()
        {
            ImGui::TextDisabled("No acceleration structures are live.\n"
                                "VkmScene::buildAccelerationStructures() creates them; this backend\n"
                                "may also report no ray tracing capability.");
        }

        // One clipped text line inside a graph node.
        void drawNodeText(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const ImVec2& position,
                          float fontSize, ImU32 colour, const char* text)
        {
            drawList->PushClipRect(min, max, true);
            drawList->AddText(ImGui::GetFont(), fontSize, position, colour, text);
            drawList->PopClipRect();
        }
    } // namespace

    void VkmAccelerationStructureInspector::draw(VkmDriverBase* driver)
    {
        if (!_visible)
        {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(820, 560), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Acceleration Structure Inspector (F4)", &_visible))
        {
            ImGui::End();
            return;
        }

        if (driver == nullptr || driver->getRenderResourcePool() == nullptr)
        {
            ImGui::TextDisabled("Driver is not available yet.");
            ImGui::End();
            return;
        }

        // Above the tab bar, so the 3D view can be toggled from whichever tab is open.
        ImGui::Checkbox("Draw in 3D scene", &_sceneOverlay);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::TextUnformatted("Outlines every top-level instance in the rendered scene: its\n"
                                   "bottom-level structure's object-space bounds as an oriented\n"
                                   "box under the instance matrix. No depth test, so a box inside\n"
                                   "geometry still shows. The selected structure is highlighted --\n"
                                   "picking a bottom-level one marks every instance of it. An\n"
                                   "instance with no recorded bounds is not drawn. Stays on while\n"
                                   "this window is closed.");
            ImGui::EndTooltip();
        }
        ImGui::Separator();

        if (ImGui::BeginTabBar("AsInspectorTabs"))
        {
            if (ImGui::BeginTabItem("Structures"))
            {
                drawStructuresTab(driver);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Graph"))
            {
                drawGraphTab(driver);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Spatial"))
            {
                drawSpatialTab(driver);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    void VkmAccelerationStructureInspector::drawStructuresTab(VkmDriverBase* driver)
    {
        VkmRenderResourcePool* pool = driver->getRenderResourcePool();
        const std::vector<StructureRow> rows = vkmCollectAccelerationStructures(pool);
        if (rows.empty())
        {
            drawEmptyHint();
            return;
        }

        size_t topLevelCount = 0;
        for (const StructureRow& row : rows)
        {
            topLevelCount += row.info._type == VkmAccelerationStructureType::TopLevel ? 1 : 0;
        }
        ImGui::Text("%zu structure(s), %zu top-level", rows.size(), topLevelCount);

        const float listHeight = ImGui::GetContentRegionAvail().y * 0.4f;
        if (ImGui::BeginTable("AsList", 6,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_SizingStretchProp,
                ImVec2(0.0f, listHeight)))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("Geometries", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Instances", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Update", ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (const StructureRow& row : rows)
            {
                const VkmAccelerationStructureInfo& info = row.info;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(row.handle.id));
                if (ImGui::Selectable(row.name.c_str(), _selected == row.handle,
                                      ImGuiSelectableFlags_SpanAllColumns))
                {
                    _selected = row.handle;
                }
                ImGui::PopID();

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(info._type == VkmAccelerationStructureType::TopLevel ? "TLAS" : "BLAS");
                ImGui::TableNextColumn();
                ImGui::Text("%zu", info._geometries.size());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", info._instances.size());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(info._allowUpdate ? "yes" : "-");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatByteSize(row.allocatedBytes).c_str());
            }
            ImGui::EndTable();
        }

        ImGui::Separator();

        const StructureRow* selected = vkmFindAccelerationStructure(rows, _selected);
        if (selected == nullptr)
        {
            ImGui::TextDisabled("Select a structure to see its instances or geometries.");
            return;
        }

        const VkmAccelerationStructureInfo& info = selected->info;
        if (info._type == VkmAccelerationStructureType::TopLevel)
        {
            ImGui::Text("%s -- %zu instance(s)", selected->name.c_str(), info._instances.size());
            if (ImGui::BeginTable("AsInstances", 3,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                    ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("Bottom-level", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < info._instances.size(); ++i)
                {
                    const VkmAccelerationStructureInstance& instance = info._instances[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", instance._instanceId);
                    ImGui::TableNextColumn();
                    const StructureRow* blasRow = vkmFindAccelerationStructure(rows, instance._blas);
                    ImGui::PushID(static_cast<int>(i));
                    // Jumps the shared selection, so the referenced structure is detailed instead.
                    if (ImGui::Selectable(blasRow != nullptr ? blasRow->name.c_str() : "<released>", false))
                    {
                        _selected = instance._blas;
                    }
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    const glm::mat4& transform = instance._transform;
                    ImGui::Text("%.2f %.2f %.2f", transform[3].x, transform[3].y, transform[3].z);
                    if (ImGui::BeginItemTooltip())
                    {
                        for (uint32_t row = 0; row < 4; ++row)
                        {
                            ImGui::Text("%8.3f %8.3f %8.3f %8.3f", transform[0][row], transform[1][row],
                                        transform[2][row], transform[3][row]);
                        }
                        ImGui::EndTooltip();
                    }
                }
                ImGui::EndTable();
            }
        }
        else
        {
            ImGui::Text("%s -- %zu geometry(ies), %u triangle(s)", selected->name.c_str(),
                        info._geometries.size(), triangleCount(info));
            if (vkmHasAccelerationStructureBounds(info))
            {
                const glm::vec3 extent = info._boundsMax - info._boundsMin;
                ImGui::Text("Bounds min (%.2f, %.2f, %.2f)  max (%.2f, %.2f, %.2f)  extent (%.2f, %.2f, %.2f)",
                            info._boundsMin.x, info._boundsMin.y, info._boundsMin.z,
                            info._boundsMax.x, info._boundsMax.y, info._boundsMax.z,
                            extent.x, extent.y, extent.z);
            }
            else
            {
                ImGui::TextDisabled("(bounds unknown -- built outside VkmScene)");
            }
            if (ImGui::BeginTable("AsGeometries", 4,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                    ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Vertices");
                ImGui::TableSetupColumn("Indices");
                ImGui::TableSetupColumn("Triangles");
                ImGui::TableSetupColumn("Stride");
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (const VkmAccelerationStructureGeometry& geometry : info._geometries)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", geometry._vertexCount);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", geometry._indexCount);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", geometry._indexCount / 3);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", geometry._vertexStride);
                }
                ImGui::EndTable();
            }
        }
    }

    void VkmAccelerationStructureInspector::drawGraphTab(VkmDriverBase* driver)
    {
        const std::vector<StructureRow> rows = vkmCollectAccelerationStructures(driver->getRenderResourcePool());
        if (rows.empty())
        {
            drawEmptyHint();
            return;
        }

        bool fitRequested = ImGui::Button("Fit");
        ImGui::SameLine();
        if (ImGui::Button("Reset view"))
        {
            _graphCanvas.resetView();
        }
        ImGui::SameLine();
        ImGui::Text("Zoom %.0f%%", _graphCanvas.getScale() * 100.0f);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::TextUnformatted("Top-level structures on the left, bottom-level on the right,\n"
                                   "one edge per instanced relationship (xN when instanced more\n"
                                   "than once). The wheel zooms about the cursor and dragging\n"
                                   "empty space pans. Click a node to select it; the Structures\n"
                                   "tab details that structure.");
            ImGui::EndTooltip();
        }

        if (_graphFittedCount != rows.size())
        {
            _graphFittedCount = rows.size();
            fitRequested = true;
        }

        // One deduplicated edge per (top-level, bottom-level) pair, counting how many
        // instances share it. Bottom-level nodes are ordered by first reference so edges
        // mostly stay short; unreferenced ones stack below.
        struct Edge
        {
            VkmResourceHandle from = VKM_INVALID_RESOURCE_HANDLE;
            VkmResourceHandle to = VKM_INVALID_RESOURCE_HANDLE;
            uint32_t count = 0;
        };
        std::vector<Edge> edges;
        std::vector<VkmResourceHandle> bottomOrder;
        std::unordered_map<VkmResourceHandle, size_t> bottomOrderIndex;
        const auto appendBottom = [&](VkmResourceHandle handle) {
            if (bottomOrderIndex.emplace(handle, bottomOrder.size()).second)
            {
                bottomOrder.push_back(handle);
            }
        };
        for (const StructureRow& row : rows)
        {
            if (row.info._type != VkmAccelerationStructureType::TopLevel)
            {
                continue;
            }
            for (const VkmAccelerationStructureInstance& instance :
                 row.info._instances)
            {
                appendBottom(instance._blas);
                const auto matches = [&](const Edge& edge) {
                    return edge.from == row.handle && edge.to == instance._blas;
                };
                const auto it = std::find_if(edges.begin(), edges.end(), matches);
                if (it != edges.end())
                {
                    ++it->count;
                }
                else
                {
                    edges.push_back(Edge{ row.handle, instance._blas, 1 });
                }
            }
        }
        for (const StructureRow& row : rows)
        {
            if (row.info._type == VkmAccelerationStructureType::BottomLevel)
            {
                appendBottom(row.handle);
            }
        }

        // Fixed two-column layout in canvas units, each column stacked with a running cursor.
        const float lineHeight = ImGui::GetTextLineHeight();
        const float nodeHeight = kNodePadding * 2.0f + 2.0f * lineHeight;
        const float bottomColumnX = kGraphMargin + kNodeWidth + kColumnGap;
        std::unordered_map<VkmResourceHandle, ImVec2> positionOf;
        float topCursor = kGraphMargin;
        for (const StructureRow& row : rows)
        {
            if (row.info._type == VkmAccelerationStructureType::TopLevel)
            {
                positionOf[row.handle] = ImVec2(kGraphMargin, topCursor);
                topCursor += nodeHeight + kNodeGap;
            }
        }
        float bottomCursor = kGraphMargin;
        for (VkmResourceHandle handle : bottomOrder)
        {
            if (vkmFindAccelerationStructure(rows, handle) != nullptr)
            {
                positionOf[handle] = ImVec2(bottomColumnX, bottomCursor);
                bottomCursor += nodeHeight + kNodeGap;
            }
        }
        if (fitRequested)
        {
            _graphCanvas.requestFit(ImVec2(bottomColumnX + kNodeWidth + kGraphMargin,
                                           std::max(topCursor, bottomCursor) + kGraphMargin));
        }

        if (!_graphCanvas.begin("AsGraphCanvas"))
        {
            _graphCanvas.end();
            return;
        }

        const float scale = _graphCanvas.getScale();
        ImDrawList* drawList = _graphCanvas.getDrawList();
        ImFont* const font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize() * scale;

        // Edges first so nodes paint over them.
        for (const Edge& edge : edges)
        {
            const auto toIt = positionOf.find(edge.to);
            if (toIt == positionOf.end())
            {
                continue;
            }
            const ImVec2 fromPosition = positionOf[edge.from];
            const ImVec2 from = _graphCanvas.toScreen(
                ImVec2(fromPosition.x + kNodeWidth, fromPosition.y + nodeHeight * 0.5f));
            const ImVec2 to = _graphCanvas.toScreen(
                ImVec2(toIt->second.x, toIt->second.y + nodeHeight * 0.5f));
            const bool touchesSelection = edge.from == _selected || edge.to == _selected;
            const float slack = std::max(24.0f * scale, (to.x - from.x) * 0.5f);
            const ImU32 colour = touchesSelection ? kSelectionColour : IM_COL32(130, 130, 145, 160);
            drawList->AddBezierCubic(from, ImVec2(from.x + slack, from.y), ImVec2(to.x - slack, to.y), to,
                                     colour, touchesSelection ? 2.5f : 1.5f);
            const float head = 5.0f * scale;
            drawList->AddTriangleFilled(ImVec2(to.x, to.y), ImVec2(to.x - head * 1.6f, to.y - head),
                                        ImVec2(to.x - head * 1.6f, to.y + head), colour);
            if (edge.count > 1)
            {
                const std::string label = "x" + std::to_string(edge.count);
                drawList->AddText(font, fontSize,
                                  ImVec2((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f - fontSize),
                                  colour, label.c_str());
            }
        }

        for (const StructureRow& row : rows)
        {
            const auto it = positionOf.find(row.handle);
            if (it == positionOf.end())
            {
                continue;
            }
            const bool topLevel = row.info._type == VkmAccelerationStructureType::TopLevel;
            const VkmAccelerationStructureInfo& info = row.info;
            const ImVec2 min = _graphCanvas.toScreen(it->second);
            const ImVec2 max(min.x + kNodeWidth * scale, min.y + nodeHeight * scale);

            ImGui::SetCursorScreenPos(min);
            ImGui::InvisibleButton(("node" + std::to_string(row.handle.id)).c_str(),
                                   ImVec2(max.x - min.x, max.y - min.y));
            if (ImGui::IsItemClicked())
            {
                _selected = row.handle;
            }

            ImU32 fill = topLevel ? IM_COL32(58, 74, 104, 255) : IM_COL32(52, 92, 72, 255);
            if (ImGui::IsItemHovered())
            {
                fill = IM_COL32(((fill >> IM_COL32_R_SHIFT) & 0xFF) + 24, ((fill >> IM_COL32_G_SHIFT) & 0xFF) + 24,
                                ((fill >> IM_COL32_B_SHIFT) & 0xFF) + 24, 255);
            }
            const bool selected = row.handle == _selected;
            drawList->AddRectFilled(min, max, fill, 5.0f);
            drawList->AddRect(min, max, selected ? kSelectionColour : IM_COL32(24, 24, 28, 255), 5.0f, 0,
                              selected ? 2.5f : 1.0f);

            const std::string title = (topLevel ? "[T] " : "[B] ") + row.name;
            const std::string subtitle = topLevel
                ? std::to_string(info._instances.size()) + " instance(s), " + formatByteSize(row.allocatedBytes)
                : std::to_string(triangleCount(info)) + " tri, " + formatByteSize(row.allocatedBytes);
            const float textX = min.x + kNodePadding * scale;
            float textY = min.y + kNodePadding * scale;
            drawNodeText(drawList, min, max, ImVec2(textX, textY), fontSize, IM_COL32(236, 236, 240, 255),
                         title.c_str());
            textY += lineHeight * scale;
            drawNodeText(drawList, min, max, ImVec2(textX, textY), fontSize, IM_COL32(176, 176, 190, 255),
                         subtitle.c_str());

            if (ImGui::IsItemHovered() && ImGui::BeginTooltip())
            {
                ImGui::Text("%s (%s)", row.name.c_str(), topLevel ? "top-level" : "bottom-level");
                ImGui::Text("%zu geometry(ies), %zu instance(s), %s", info._geometries.size(),
                            info._instances.size(), formatByteSize(row.allocatedBytes).c_str());
                ImGui::EndTooltip();
            }
        }

        _graphCanvas.end();
    }

    void VkmAccelerationStructureInspector::drawSpatialTab(VkmDriverBase* driver)
    {
        const std::vector<StructureRow> rows = vkmCollectAccelerationStructures(driver->getRenderResourcePool());
        if (rows.empty())
        {
            drawEmptyHint();
            return;
        }

        // Every instance of every top-level structure. An instance whose structure has no
        // recorded bounds keeps only its translation and is drawn as a marker; the rest carry
        // their bottom-level bounds into world space, re-fitted to the world axes by
        // VkmSceneAABB::transformed (the 3D overlay draws the same instances as true oriented
        // boxes instead).
        const std::vector<VkmAccelerationStructureInstanceBox> instances =
            vkmCollectInstanceBoxes(rows);
        struct InstanceBox
        {
            VkmResourceHandle tlas = VKM_INVALID_RESOURCE_HANDLE;
            VkmResourceHandle blas = VKM_INVALID_RESOURCE_HANDLE;
            uint32_t instanceId = 0;
            VkmSceneAABB world;
            glm::vec3 origin{ 0.0f };
        };
        std::vector<InstanceBox> boxes;
        boxes.reserve(instances.size());
        VkmSceneAABB unionBounds;
        for (const VkmAccelerationStructureInstanceBox& instance : instances)
        {
            InstanceBox box;
            box.tlas = instance.tlas;
            box.blas = instance.blas;
            box.instanceId = instance.instanceId;
            box.origin = glm::vec3(instance.transform[3]);
            if (instance.hasBounds)
            {
                box.world = VkmSceneAABB{ instance.boundsMin, instance.boundsMax, true }
                                .transformed(instance.transform);
                unionBounds.expand(box.world);
            }
            else
            {
                unionBounds.expand(box.origin);
            }
            boxes.push_back(box);
        }
        if (boxes.empty())
        {
            ImGui::TextDisabled("No top-level instances to draw.");
            return;
        }

        bool fitRequested = ImGui::Button("Fit");
        ImGui::SameLine();
        if (ImGui::Button("Reset view"))
        {
            _spatialCanvas.resetView();
        }
        ImGui::SameLine();
        ImGui::Text("Zoom %.0f%%", _spatialCanvas.getScale() * 100.0f);
        ImGui::SameLine();
        ImGui::TextDisabled("world XZ, +X right, +Z down");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::TextUnformatted("Top-down world XZ. One rectangle per top-level instance: its\n"
                                   "bottom-level structure's object-space bounds transformed by the\n"
                                   "instance matrix. A dot marks an instance whose structure has no\n"
                                   "recorded bounds. Click a rectangle to select its bottom-level\n"
                                   "structure; the Structures tab details it.\n"
                                   "These rectangles are re-fitted to the world axes, so a rotated\n"
                                   "instance reads larger than it is; \"Draw in 3D scene\" shows the\n"
                                   "same instances as oriented boxes with their height.");
            ImGui::EndTooltip();
        }

        if (_spatialFittedCount != boxes.size())
        {
            _spatialFittedCount = boxes.size();
            fitRequested = true;
        }

        // World XZ to canvas units: union-min at the margin, Z down the canvas.
        const auto toCanvas = [&](float worldX, float worldZ) {
            return ImVec2((worldX - unionBounds._min.x) * kUnitsPerMeter + kGraphMargin,
                          (worldZ - unionBounds._min.z) * kUnitsPerMeter + kGraphMargin);
        };
        if (fitRequested)
        {
            const glm::vec3 extent = unionBounds.getExtent();
            _spatialCanvas.requestFit(ImVec2(extent.x * kUnitsPerMeter + 2.0f * kGraphMargin,
                                             extent.z * kUnitsPerMeter + 2.0f * kGraphMargin));
        }

        if (!_spatialCanvas.begin("AsSpatialCanvas"))
        {
            _spatialCanvas.end();
            return;
        }

        const float scale = _spatialCanvas.getScale();
        ImDrawList* drawList = _spatialCanvas.getDrawList();
        ImFont* const font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize() * scale;

        for (size_t i = 0; i < boxes.size(); ++i)
        {
            const InstanceBox& box = boxes[i];
            const bool selected = box.blas == _selected || box.tlas == _selected;
            const StructureRow* blasRow = vkmFindAccelerationStructure(rows, box.blas);
            const std::string label = "#" + std::to_string(box.instanceId) +
                                      (blasRow != nullptr ? " " + blasRow->name : "");

            if (box.world._valid)
            {
                const ImVec2 min = _spatialCanvas.toScreen(toCanvas(box.world._min.x, box.world._min.z));
                const ImVec2 max = _spatialCanvas.toScreen(toCanvas(box.world._max.x, box.world._max.z));

                ImGui::SetCursorScreenPos(min);
                ImGui::InvisibleButton(("box" + std::to_string(i)).c_str(),
                                       ImVec2(std::max(max.x - min.x, 1.0f), std::max(max.y - min.y, 1.0f)));
                if (ImGui::IsItemClicked())
                {
                    _selected = box.blas;
                }

                const ImU32 outline = selected ? kSelectionColour : IM_COL32(150, 190, 220, 220);
                ImU32 fill = selected ? IM_COL32(255, 200, 90, 40) : IM_COL32(120, 160, 200, 30);
                if (ImGui::IsItemHovered())
                {
                    fill = IM_COL32(160, 200, 240, 70);
                }
                drawList->AddRectFilled(min, max, fill);
                drawList->AddRect(min, max, outline, 0.0f, 0, selected ? 2.5f : 1.0f);

                if (ImGui::CalcTextSize(label.c_str()).x * scale < max.x - min.x)
                {
                    drawList->PushClipRect(min, max, true);
                    drawList->AddText(font, fontSize, ImVec2(min.x + 2.0f * scale, min.y + 2.0f * scale),
                                      IM_COL32(220, 228, 236, 255), label.c_str());
                    drawList->PopClipRect();
                }
            }
            else
            {
                const ImVec2 centre = _spatialCanvas.toScreen(toCanvas(box.origin.x, box.origin.z));
                const float radius = std::max(4.0f * scale, 2.0f);
                ImGui::SetCursorScreenPos(ImVec2(centre.x - radius, centre.y - radius));
                ImGui::InvisibleButton(("box" + std::to_string(i)).c_str(),
                                       ImVec2(radius * 2.0f, radius * 2.0f));
                if (ImGui::IsItemClicked())
                {
                    _selected = box.blas;
                }
                drawList->AddCircleFilled(centre, radius,
                                          selected ? kSelectionColour : IM_COL32(200, 160, 120, 255));
                drawList->AddText(font, fontSize, ImVec2(centre.x + 6.0f * scale, centre.y - fontSize * 0.5f),
                                  IM_COL32(200, 200, 210, 255), label.c_str());
            }

            if (ImGui::IsItemHovered() && ImGui::BeginTooltip())
            {
                ImGui::TextUnformatted(label.c_str());
                ImGui::Text("origin (%.2f, %.2f, %.2f)", box.origin.x, box.origin.y, box.origin.z);
                if (box.world._valid)
                {
                    const glm::vec3 extent = box.world.getExtent();
                    ImGui::Text("world extent (%.2f, %.2f, %.2f)", extent.x, extent.y, extent.z);
                }
                ImGui::EndTooltip();
            }
        }

        _spatialCanvas.end();
    }
} // namespace vkm
