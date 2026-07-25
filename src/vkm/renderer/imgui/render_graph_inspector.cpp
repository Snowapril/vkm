// Copyright (c) 2025 Snowapril

#include <vkm/renderer/imgui/render_graph_inspector.h>
#include <vkm/renderer/imgui/imgui_renderer.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph_capture.h>
#include <vkm/renderer/backend/common/render_graph.h>

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace vkm
{
    namespace
    {
        const char* loadActionToString(VkmLoadAction action)
        {
            switch (action)
            {
                case VkmLoadAction::Load:     return "Load";
                case VkmLoadAction::Clear:    return "Clear";
                case VkmLoadAction::DontCare: return "DontCare";
            }
            return "?";
        }

        const char* storeActionToString(VkmStoreAction action)
        {
            switch (action)
            {
                case VkmStoreAction::Store:    return "Store";
                case VkmStoreAction::DontCare: return "DontCare";
            }
            return "?";
        }

        const char* subGraphTypeTag(VkmRenderSubGraphType type)
        {
            switch (type)
            {
                case VkmRenderSubGraphType::Graphics: return "G";
                case VkmRenderSubGraphType::Compute:  return "C";
                case VkmRenderSubGraphType::Transfer: return "T";
            }
            return "?";
        }

        /*
        * @brief Preview one captured texture. Prefers the capture's own frame-accurate
        * snapshot; falls back to the live texture when there is none, which is the case for
        * cube/array/depth sources and on backends without copyTexture. `unavailableReason` is
        * shown when neither is displayable.
        */
        void drawTexturePreview(const VkmCapturedResourceInfo& info, VkmImGuiRendererBase* imGuiRenderer,
                                const char* unavailableReason)
        {
            const bool isSnapshot = info.snapshotTexture.isValid();
            // A cube or array texture cannot be bound to ImGui's texture2d pipeline, so
            // without a snapshot there is nothing safe to draw -- the Textures tab is where
            // those are inspected face by face.
            if (!isSnapshot && (info.textureType == VkmTextureType::Cube || info.numArrayLayers > 1))
            {
                ImGui::TextDisabled("(%s -- inspect it face by face in the Textures tab)",
                                    info.textureType == VkmTextureType::Cube ? "cubemap" : "texture array");
                return;
            }

            const uint64_t textureID =
                imGuiRenderer->getTextureID(isSnapshot ? info.snapshotTexture : info.handle);
            if (textureID == 0)
            {
                ImGui::TextDisabled("%s", unavailableReason);
                return;
            }

            const float srcWidth = static_cast<float>(info.extent.x);
            const float srcHeight = static_cast<float>(info.extent.y);
            const float maxPreviewWidth = 384.0f;
            const float scale = (srcWidth > maxPreviewWidth) ? maxPreviewWidth / srcWidth : 1.0f;
            ImGui::Image(static_cast<ImTextureID>(textureID), ImVec2(srcWidth * scale, srcHeight * scale));
            if (!isSnapshot)
            {
                ImGui::TextDisabled("(live contents -- not the captured frame)");
            }
        }

        void drawBufferHexView(const VkmCapturedBuffer& buffer)
        {
            ImGui::Text("Captured %zu of %llu bytes", buffer.data.size(),
                        static_cast<unsigned long long>(buffer.info.size));
            constexpr int kBytesPerRow = 16;
            const int rowCount = static_cast<int>((buffer.data.size() + kBytesPerRow - 1) / kBytesPerRow);

            ImGui::BeginChild("BufferHexView", ImVec2(0, 200), ImGuiChildFlags_Borders);
            ImGuiListClipper clipper;
            clipper.Begin(rowCount);
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const size_t offset = static_cast<size_t>(row) * kBytesPerRow;
                    const size_t rowBytes = std::min<size_t>(kBytesPerRow, buffer.data.size() - offset);

                    std::string hex, ascii;
                    hex.reserve(kBytesPerRow * 3);
                    ascii.reserve(kBytesPerRow);
                    for (size_t i = 0; i < rowBytes; ++i)
                    {
                        const uint8_t byte = buffer.data[offset + i];
                        char hexByte[4];
                        std::snprintf(hexByte, sizeof(hexByte), "%02X ", byte);
                        hex += hexByte;
                        ascii += (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
                    }
                    ImGui::Text("%08zX  %-48s %s", offset, hex.c_str(), ascii.c_str());
                }
            }
            clipper.End();
            ImGui::EndChild();
        }
    }

    void VkmRenderGraphInspector::draw(VkmRenderGraphCapture& capture, VkmDriverBase* driver,
                                       VkmImGuiRendererBase* imGuiRenderer,
                                       VkmPipelineStateManager* pipelineStateManager)
    {
        if (!_visible)
        {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(820, 560), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Render Graph Inspector (F7)", &_visible))
        {
            ImGui::End();
            return;
        }

        if (pipelineStateManager == nullptr)
        {
            ImGui::TextDisabled("Pipeline state manager is not available yet.");
            ImGui::End();
            return;
        }

        if (ImGui::BeginTabBar("InspectorTabs"))
        {
            if (ImGui::BeginTabItem("Capture"))
            {
                drawCaptureTab(capture, imGuiRenderer, pipelineStateManager);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Pipelines"))
            {
                drawPipelinesTab(pipelineStateManager);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Textures"))
            {
                _textureBrowser.draw(driver, imGuiRenderer);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    void VkmRenderGraphInspector::drawCaptureTab(VkmRenderGraphCapture& capture,
                                                 VkmImGuiRendererBase* imGuiRenderer,
                                                 VkmPipelineStateManager* pipelineStateManager)
    {
        if (capture.getState() == VkmRenderGraphCapture::State::Pending)
        {
            ImGui::TextUnformatted("Capturing...");
            return;
        }
        if (capture.getState() != VkmRenderGraphCapture::State::Ready)
        {
            ImGui::TextDisabled("No capture yet.");
            if (ImGui::Button("Capture (F10)"))
            {
                capture.arm();
            }
            return;
        }

        ImGui::Text("Captured frame slot %u -- %zu passes", capture.getCapturedFrameIndex(), capture.getPasses().size());
        ImGui::SameLine();
        if (ImGui::Button("Recapture (F10)"))
        {
            capture.arm();
        }
        if (!capture.hasContentCapture())
        {
            ImGui::TextDisabled("Content capture (texture/buffer contents) is unavailable on this backend.");
        }
        ImGui::Separator();

        const std::vector<VkmCapturedPass>& passes = capture.getPasses();
        _selectedPass = passes.empty() ? 0 : std::clamp(_selectedPass, 0, static_cast<int>(passes.size()) - 1);

        // Left: pass list
        ImGui::BeginChild("PassList", ImVec2(220, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        for (int i = 0; i < static_cast<int>(passes.size()); ++i)
        {
            const VkmCapturedPass& pass = passes[i];
            const std::string label =
                "[" + std::string(subGraphTypeTag(pass.type)) + "] " + pass.name + " (#" + std::to_string(pass.subGraphId) + ")";
            if (ImGui::Selectable(label.c_str(), _selectedPass == i))
            {
                _selectedPass = i;
                _selectedBuffer = -1;
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        // Right: selected pass details
        ImGui::BeginChild("PassDetails", ImVec2(0, 0));
        if (_selectedPass >= 0 && _selectedPass < static_cast<int>(passes.size()))
        {
            const VkmCapturedPass& pass = passes[_selectedPass];
            ImGui::Text("%s (#%u)", pass.name.c_str(), pass.subGraphId);
            if (pass.type == VkmRenderSubGraphType::Graphics)
            {
                ImGui::Text("Graphics pass, %ux%u", pass.width, pass.height);
            }

            if (!pass.dependencies.empty())
            {
                std::string deps;
                for (uint32_t dep : pass.dependencies)
                {
                    deps += (deps.empty() ? "#" : ", #") + std::to_string(dep);
                }
                ImGui::Text("Depends on: %s", deps.c_str());
            }

            if (ImGui::CollapsingHeader("Pipelines", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (pass.pipelineNames.empty())
                {
                    ImGui::TextDisabled("(no pipelines bound)");
                }
                for (const std::string& pipelineName : pass.pipelineNames)
                {
                    ImGui::BulletText("%s", pipelineName.c_str());
                    const size_t sourceIndex = pipelineStateManager->findSourceIndexOfPipelineState(pipelineName);
                    if (sourceIndex == std::string::npos)
                    {
                        continue;
                    }
                    ImGui::SameLine();
                    ImGui::PushID(pipelineName.c_str());
                    if (ImGui::SmallButton("Reload"))
                    {
                        reloadSource(pipelineStateManager, sourceIndex);
                    }
                    ImGui::PopID();
                }
            }

            if (ImGui::CollapsingHeader("Outputs", ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (size_t i = 0; i < pass.colorAttachments.size(); ++i)
                {
                    const VkmCapturedAttachment& attachment = pass.colorAttachments[i];
                    ImGui::PushID(static_cast<int>(i));
                    const std::string header = "Color " + std::to_string(i) +
                        (attachment.info.debugName.empty() ? "" : " -- " + attachment.info.debugName);
                    if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        ImGui::Text("%s, %ux%u, load %s / store %s",
                                    vkmFormatName(attachment.info.format),
                                    attachment.info.extent.x, attachment.info.extent.y,
                                    loadActionToString(attachment.loadAction),
                                    storeActionToString(attachment.storeAction));
                        drawTexturePreview(attachment.info, imGuiRenderer,
                            attachment.isPresentTarget ? "(swapchain back buffer -- not captured)"
                                                       : "(content preview unavailable on this backend)");
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                if (pass.depthStencilAttachment.has_value())
                {
                    const VkmCapturedAttachment& attachment = *pass.depthStencilAttachment;
                    const std::string header = std::string("DepthStencil") +
                        (attachment.info.debugName.empty() ? "" : " -- " + attachment.info.debugName);
                    if (ImGui::TreeNodeEx(header.c_str()))
                    {
                        ImGui::Text("%s, %ux%u, load %s / store %s (metadata only)",
                                    vkmFormatName(attachment.info.format),
                                    attachment.info.extent.x, attachment.info.extent.y,
                                    loadActionToString(attachment.loadAction),
                                    storeActionToString(attachment.storeAction));
                        ImGui::TreePop();
                    }
                }
                if (pass.colorAttachments.empty() && !pass.depthStencilAttachment.has_value())
                {
                    ImGui::TextDisabled("(no attachments)");
                }
            }

            if (ImGui::CollapsingHeader("Inputs (referenced resources)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (pass.referencedResources.empty())
                {
                    ImGui::TextDisabled("(none declared)");
                }
                for (size_t i = 0; i < pass.referencedResources.size(); ++i)
                {
                    const VkmCapturedResourceInfo& resource = pass.referencedResources[i];
                    if (resource.type != VkmResourceType::Texture)
                    {
                        ImGui::BulletText("%s '%s' (%llu bytes)", vkmResourceTypeName(resource.type),
                                          resource.debugName.c_str(),
                                          static_cast<unsigned long long>(resource.size));
                        continue;
                    }

                    ImGui::PushID(static_cast<int>(i));
                    const std::string header = "Texture " + std::to_string(i) +
                        (resource.debugName.empty() ? "" : " -- " + resource.debugName);
                    if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        const std::string layers = resource.numArrayLayers > 1
                            ? " x " + std::to_string(resource.numArrayLayers) + " layers"
                            : std::string();
                        ImGui::Text("%s, %ux%u%s", vkmFormatName(resource.format),
                                    resource.extent.x, resource.extent.y, layers.c_str());
                        drawTexturePreview(resource, imGuiRenderer,
                                           "(content preview unavailable on this backend)");
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            }

            if (!pass.capturedBuffers.empty() && ImGui::CollapsingHeader("Buffer contents", ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (int i = 0; i < static_cast<int>(pass.capturedBuffers.size()); ++i)
                {
                    const VkmCapturedBuffer& buffer = pass.capturedBuffers[i];
                    const std::string label = buffer.info.debugName.empty()
                        ? "Buffer " + std::to_string(i)
                        : buffer.info.debugName;
                    if (ImGui::Selectable(label.c_str(), _selectedBuffer == i))
                    {
                        _selectedBuffer = (_selectedBuffer == i) ? -1 : i;
                    }
                }
                if (_selectedBuffer >= 0 && _selectedBuffer < static_cast<int>(pass.capturedBuffers.size()))
                {
                    const VkmCapturedBuffer& buffer = pass.capturedBuffers[_selectedBuffer];
                    if (buffer.data.empty())
                    {
                        ImGui::TextDisabled("(buffer contents unavailable)");
                    }
                    else
                    {
                        drawBufferHexView(buffer);
                    }
                }
            }
        }
        ImGui::EndChild();
    }

    void VkmRenderGraphInspector::reloadSource(VkmPipelineStateManager* pipelineStateManager, size_t sourceIndex)
    {
        const bool recompile = _recompileShadersOnReload && VkmPipelineStateManager::isShaderRecompilationAvailable();
        std::string error;
        _reloadFailed = !pipelineStateManager->reloadSource(sourceIndex, recompile, &error);
        // A successful reload can still report something worth reading (a variant the edit
        // removed), so show whatever came back either way.
        _reloadStatus = error.empty() ? (_reloadFailed ? "Reload failed." : "Reloaded.") : error;
    }

    void VkmRenderGraphInspector::drawPipelinesTab(VkmPipelineStateManager* pipelineStateManager)
    {
        const bool recompileAvailable = VkmPipelineStateManager::isShaderRecompilationAvailable();

        if (ImGui::Button("Reload All"))
        {
            const bool recompile = _recompileShadersOnReload && recompileAvailable;
            std::string error;
            _reloadFailed = !pipelineStateManager->reloadAllSources(recompile, &error);
            _reloadStatus = error.empty() ? (_reloadFailed ? "Reload failed." : "Reloaded all sources.") : error;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!recompileAvailable);
        ImGui::Checkbox("Recompile shaders", &_recompileShadersOnReload);
        ImGui::EndDisabled();
        ImGui::SameLine();
        bool autoReload = pipelineStateManager->isAutoReloadOnChange();
        if (ImGui::Checkbox("Auto-reload on change", &autoReload))
        {
            pipelineStateManager->setAutoReloadOnChange(autoReload);
        }

        if (!recompileAvailable)
        {
            ImGui::TextDisabled("vkm-compiler is not available in this build -- Reload re-applies "
                                "json render state only, shader edits need a rebuild.");
        }
        ImGui::Separator();

        const std::vector<VkmPipelineStateSource>& sources = pipelineStateManager->getSources();
        if (sources.empty())
        {
            ImGui::TextDisabled("(no pipeline state json loaded)");
            return;
        }

        if (ImGui::BeginTable("PipelineSources", 4,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Variants", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < sources.size(); ++i)
            {
                const VkmPipelineStateSource& source = sources[i];
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                ImGui::TableNextColumn();
                if (source.jsonPath.empty())
                {
                    ImGui::TextDisabled("(registered from a descriptor)");
                }
                else
                {
                    ImGui::TextUnformatted(std::filesystem::path(source.jsonPath).filename().string().c_str());
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s", source.jsonPath.c_str());
                    }
                }

                ImGui::TableNextColumn();
                std::string variants;
                for (const std::string& name : source.variantNames)
                {
                    variants += (variants.empty() ? "" : ", ") + name;
                }
                ImGui::TextUnformatted(variants.c_str());

                ImGui::TableNextColumn();
                if (source.stale)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "stale");
                }
                else
                {
                    ImGui::TextDisabled("up to date");
                }

                ImGui::TableNextColumn();
                ImGui::BeginDisabled(source.jsonPath.empty());
                if (ImGui::SmallButton("Reload"))
                {
                    reloadSource(pipelineStateManager, i);
                }
                ImGui::EndDisabled();

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Compiler stderr is multi-line and is the whole point of showing this, so give it a
        // scrollable box rather than a single truncated line.
        const std::string& status = pipelineStateManager->getLastReloadError().empty()
            ? _reloadStatus : pipelineStateManager->getLastReloadError();
        if (!status.empty())
        {
            ImGui::Separator();
            ImGui::TextColored(_reloadFailed ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                               "Last reload");
            ImGui::BeginChild("ReloadStatus", ImVec2(0, 120), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", status.c_str());
            ImGui::EndChild();
        }
    }
}
