// Copyright (c) 2025 Snowapril

#include <vkm/renderer/imgui/texture_browser.h>
#include <vkm/renderer/imgui/imgui_renderer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/memory_report.h>

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace vkm
{
    namespace
    {
        constexpr float kMaxPreviewWidth = 384.0f;

        struct TextureRow
        {
            VkmResourceHandle handle = VKM_INVALID_RESOURCE_HANDLE;
            std::string name;
            const VkmTextureInfo* info = nullptr;
            uint64_t allocatedBytes = 0;
        };

        const char* textureTypeLabel(const VkmTextureInfo& info)
        {
            if (info._type == VkmTextureType::Cube)
            {
                return "Cube";
            }
            return (info._numArrayLayers > 1) ? "2D array" : "2D";
        }

        // Compact one-letter summary of the usage bitmask, so the column stays narrow:
        // Sampled, Storage, Color, Depth, Present, transfer Src, transfer Dst.
        std::string usageLabel(const VkmTextureInfo& info)
        {
            std::string label;
            const auto append = [&label](bool set, char letter) { label.push_back(set ? letter : '-'); };
            append((info._flags & VkmResourceCreateInfo::AllowShaderRead) != 0, 'S');
            append((info._flags & VkmResourceCreateInfo::AllowShaderWrite) != 0, 'W');
            append((info._flags & VkmResourceCreateInfo::AllowColorAttachment) != 0, 'C');
            append((info._flags & VkmResourceCreateInfo::AllowDepthStencilAttachment) != 0, 'D');
            append((info._flags & VkmResourceCreateInfo::AllowPresent) != 0, 'P');
            append((info._flags & VkmResourceCreateInfo::AllowTransferSrc) != 0, 's');
            append((info._flags & VkmResourceCreateInfo::AllowTransferDst) != 0, 'd');
            return label;
        }

        // A cube or array texture cannot be bound to the ImGui pipeline, which samples a
        // texture2d, so it goes through the per-layer readback preview instead.
        bool isDirectlyDisplayable(const VkmTextureInfo& info)
        {
            return info._type != VkmTextureType::Cube && info._numArrayLayers == 1 &&
                   info._extent.z <= 1;
        }

        // Depth/stencil has nothing ImGui's color pipeline can interpret, and an unknown
        // format has no byte size to read back -- both are metadata only.
        bool isPreviewableFormat(VkmFormat format)
        {
            return !hasDepth(format) && !hasStencil(format) && vkmBytesPerTexel(format) != 0;
        }
    } // namespace

    void VkmTextureBrowser::draw(VkmDriverBase* driver, VkmImGuiRendererBase* imGuiRenderer)
    {
        _sampledTextures.clear();

        VkmRenderResourcePool* pool = driver->getRenderResourcePool();
        const std::vector<VkmResourceHandle> handles = pool->getAllResourceHandles(VkmResourceType::Texture);

        std::vector<TextureRow> rows;
        rows.reserve(handles.size());
        for (VkmResourceHandle handle : handles)
        {
            VkmTexture* texture = pool->getResource<VkmTexture>(handle);
            if (texture == nullptr)
            {
                continue;
            }

            TextureRow row;
            row.handle = handle;
            row.info = &texture->getTextureInfo();
            // The durable copy of the name, not VkmTextureInfo::_debugName -- that is a
            // borrowed const char* whose owner may be long gone.
            const std::optional<VkmResourceMemoryTag> tag = pool->getResourceMemoryTag(handle);
            if (tag.has_value() && !tag->name.empty())
            {
                row.name = tag->name;
                row.allocatedBytes = tag->allocatedSize;
            }
            else
            {
                row.name = "<unnamed #" + std::to_string(handle.id) + ">";
            }

            if (_nameFilter[0] != '\0' && row.name.find(_nameFilter) == std::string::npos)
            {
                continue;
            }
            rows.push_back(std::move(row));
        }

        ImGui::Text("%zu texture(s) live", handles.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##TextureFilter", "filter by name", _nameFilter, sizeof(_nameFilter));

        const float listHeight = ImGui::GetContentRegionAvail().y * 0.45f;
        if (ImGui::BeginTable("TextureList", 7,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_SizingStretchProp,
                ImVec2(0.0f, listHeight)))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Mips", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("Usage", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (const TextureRow& row : rows)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(row.handle.id));
                if (ImGui::Selectable(row.name.c_str(), _selected == row.handle,
                                      ImGuiSelectableFlags_SpanAllColumns))
                {
                    _selected = row.handle;
                    _selectedLayer = 0;
                    _layerPreviewSource = VKM_INVALID_RESOURCE_HANDLE; // force a refresh
                }
                ImGui::PopID();

                ImGui::TableNextColumn();
                ImGui::Text("%ux%u", row.info->_extent.x, row.info->_extent.y);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(vkmFormatName(row.info->_format));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(textureTypeLabel(*row.info));
                ImGui::TableNextColumn();
                ImGui::Text("%u", row.info->_numMipLevels);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(usageLabel(*row.info).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatByteSize(row.allocatedBytes).c_str());
            }
            ImGui::EndTable();
        }

        ImGui::Separator();

        VkmTexture* selected = pool->getResource<VkmTexture>(_selected);
        if (selected == nullptr)
        {
            ImGui::TextDisabled("Select a texture to preview its contents.");
            return;
        }
        const VkmTextureInfo& info = selected->getTextureInfo();

        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Zoom", &_previewScale, 0.1f, 4.0f, "%.2fx");

        if (!isPreviewableFormat(info._format))
        {
            ImGui::TextDisabled("(no preview for %s -- depth/stencil formats are metadata only)",
                                vkmFormatName(info._format));
            return;
        }

        VkmResourceHandle displayed = _selected;
        if (!isDirectlyDisplayable(info))
        {
            // A face/layer at a time, through a CPU round trip: the ImGui pipeline binds a
            // texture2d, so there is nothing a cube or array texture can be bound to directly.
            const uint32_t layerCount = std::max<uint32_t>(info._numArrayLayers, 1u);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            int layer = static_cast<int>(_selectedLayer);
            const bool layerChanged =
                ImGui::SliderInt("Layer", &layer, 0, static_cast<int>(layerCount) - 1);
            _selectedLayer = static_cast<uint32_t>(std::clamp(layer, 0, static_cast<int>(layerCount) - 1));

            ImGui::SameLine();
            const bool refreshRequested = ImGui::Button("Refresh");
            const bool needsRefresh = layerChanged || refreshRequested ||
                                      _layerPreviewSource != _selected;
            // Never per frame: the readback blocks on a full queue wait.
            if (needsRefresh && !refreshLayerPreview(driver, _selected, _selectedLayer))
            {
                ImGui::TextDisabled("(layer preview unavailable for this texture)");
                return;
            }
            if (!_layerPreview.isValid())
            {
                ImGui::TextDisabled("(layer preview unavailable for this texture)");
                return;
            }
            displayed = _layerPreview;
        }

        const uint64_t textureID = imGuiRenderer->getTextureID(displayed);
        if (textureID == 0)
        {
            ImGui::TextDisabled("(content preview unavailable on this backend)");
            return;
        }

        const float srcWidth = static_cast<float>(info._extent.x);
        const float srcHeight = static_cast<float>(info._extent.y);
        const float fit = (srcWidth > kMaxPreviewWidth) ? kMaxPreviewWidth / srcWidth : 1.0f;
        const float scale = fit * _previewScale;
        ImGui::Image(static_cast<ImTextureID>(textureID), ImVec2(srcWidth * scale, srcHeight * scale));
        _sampledTextures.push_back(displayed);

        if (displayed == _selected)
        {
            ImGui::TextDisabled("Live contents -- a render target written this frame may tear.");
        }
    }

    bool VkmTextureBrowser::refreshLayerPreview(VkmDriverBase* driver, VkmResourceHandle source, uint32_t layer)
    {
        VkmTexture* sourceTexture = driver->getRenderResourcePool()->getResource<VkmTexture>(source);
        if (sourceTexture == nullptr)
        {
            return false;
        }
        const VkmTextureInfo& sourceInfo = sourceTexture->getTextureInfo();

        const VkmTextureReadbackResult readback = driver->readbackTexture(source, layer);
        if (readback.pixels.empty())
        {
            return false;
        }

        // Recreate only when the shape changed. The preview keeps the source's own format so
        // the readback's native channel order (BGRA for a BGRA8 source) needs no swizzle.
        if (!_layerPreview.isValid() || _layerPreviewFormat != sourceInfo._format ||
            _layerPreviewExtent.x != sourceInfo._extent.x || _layerPreviewExtent.y != sourceInfo._extent.y)
        {
            releaseResources(driver);

            _layerPreviewName = "TextureBrowser.LayerPreview";
            VkmTextureInfo previewInfo{};
            previewInfo._flags = VkmResourceCreateInfo::AllowTransferDst | VkmResourceCreateInfo::AllowShaderRead;
            previewInfo._extent = glm::uvec3(sourceInfo._extent.x, sourceInfo._extent.y, 1);
            previewInfo._numMipLevels = 1;
            previewInfo._numArrayLayers = 1;
            previewInfo._format = sourceInfo._format;
            previewInfo._debugName = _layerPreviewName.c_str();

            VkmTexture* previewTexture = driver->newTexture(previewInfo);
            if (previewTexture == nullptr)
            {
                return false;
            }
            _layerPreview = previewTexture->getHandle();
            _layerPreviewFormat = sourceInfo._format;
            _layerPreviewExtent = previewInfo._extent;
        }

        if (!driver->uploadToTexture(_layerPreview, readback.pixels.data(), readback.pixels.size()))
        {
            return false;
        }
        _layerPreviewSource = source;
        return true;
    }

    void VkmTextureBrowser::releaseResources(VkmDriverBase* driver)
    {
        if (_layerPreview.isValid())
        {
            driver->getDeferredReclaimer()->requestRelease(_layerPreview);
            _layerPreview = VKM_INVALID_RESOURCE_HANDLE;
        }
        _layerPreviewSource = VKM_INVALID_RESOURCE_HANDLE;
        _layerPreviewFormat = VkmFormat::Undefined;
        _layerPreviewExtent = glm::uvec3(0, 0, 0);
    }
} // namespace vkm
