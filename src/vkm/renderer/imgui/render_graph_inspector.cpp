// Copyright (c) 2025 Snowapril

#include <vkm/renderer/imgui/render_graph_inspector.h>
#include <vkm/renderer/imgui/imgui_renderer.h>
#include <vkm/renderer/backend/common/render_graph_capture.h>
#include <vkm/renderer/backend/common/render_graph.h>

#include <imgui.h>

#include <algorithm>
#include <string>

namespace vkm
{
    namespace
    {
        const char* formatToString(VkmFormat format)
        {
            switch (format)
            {
                case VkmFormat::R8G8B8A8_UNORM:      return "R8G8B8A8_UNORM";
                case VkmFormat::R8G8B8A8_SRGB:       return "R8G8B8A8_SRGB";
                case VkmFormat::R8G8B8A8_UINT:       return "R8G8B8A8_UINT";
                case VkmFormat::R8G8B8A8_SNORM:      return "R8G8B8A8_SNORM";
                case VkmFormat::R8G8B8A8_SINT:       return "R8G8B8A8_SINT";
                case VkmFormat::R16G16B16A16_UNORM:  return "R16G16B16A16_UNORM";
                case VkmFormat::R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
                case VkmFormat::R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
                case VkmFormat::D32_SFLOAT:          return "D32_SFLOAT";
                case VkmFormat::D24_UNORM_S8_UINT:   return "D24_UNORM_S8_UINT";
                case VkmFormat::D32_SFLOAT_S8_UINT:  return "D32_SFLOAT_S8_UINT";
                case VkmFormat::BGRA8_UNORM:         return "BGRA8_UNORM";
                case VkmFormat::BGRA8_SRGB:          return "BGRA8_SRGB";
                case VkmFormat::Undefined:
                default:                             return "Undefined";
            }
        }

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

        const char* resourceTypeToString(VkmResourceType type)
        {
            switch (type)
            {
                case VkmResourceType::Texture:       return "Texture";
                case VkmResourceType::Buffer:        return "Buffer";
                case VkmResourceType::StagingBuffer: return "StagingBuffer";
                case VkmResourceType::Sampler:       return "Sampler";
                case VkmResourceType::TextureView:   return "TextureView";
                case VkmResourceType::BufferView:    return "BufferView";
                default:                             return "Undefined";
            }
        }

        void drawAttachmentPreview(const VkmCapturedAttachment& attachment, VkmImGuiRendererBase* imGuiRenderer)
        {
            if (!attachment.snapshotTexture.isValid())
            {
                ImGui::TextDisabled(attachment.isPresentTarget ? "(swapchain back buffer -- not captured)"
                                                               : "(content preview unavailable)");
                return;
            }
            const uint64_t textureID = imGuiRenderer->getTextureID(attachment.snapshotTexture);
            if (textureID == 0)
            {
                ImGui::TextDisabled("(content preview unavailable on this backend)");
                return;
            }

            const float srcWidth = static_cast<float>(attachment.info.extent.x);
            const float srcHeight = static_cast<float>(attachment.info.extent.y);
            const float maxPreviewWidth = 384.0f;
            const float scale = (srcWidth > maxPreviewWidth) ? maxPreviewWidth / srcWidth : 1.0f;
            ImGui::Image(static_cast<ImTextureID>(textureID), ImVec2(srcWidth * scale, srcHeight * scale));
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
                                       VkmImGuiRendererBase* imGuiRenderer)
    {
        (void)driver;

        if (capture.getState() == VkmRenderGraphCapture::State::Pending)
        {
            ImGui::Begin("Render Graph Inspector");
            ImGui::TextUnformatted("Capturing...");
            ImGui::End();
            return;
        }
        if (capture.getState() != VkmRenderGraphCapture::State::Ready)
        {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(760, 480), ImGuiCond_FirstUseEver);
        ImGui::Begin("Render Graph Inspector");

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
                                    formatToString(attachment.info.format),
                                    attachment.info.extent.x, attachment.info.extent.y,
                                    loadActionToString(attachment.loadAction),
                                    storeActionToString(attachment.storeAction));
                        drawAttachmentPreview(attachment, imGuiRenderer);
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
                                    formatToString(attachment.info.format),
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
                for (const VkmCapturedResourceInfo& resource : pass.referencedResources)
                {
                    if (resource.type == VkmResourceType::Texture)
                    {
                        ImGui::BulletText("%s '%s' (%s, %ux%u)", resourceTypeToString(resource.type),
                                          resource.debugName.c_str(), formatToString(resource.format),
                                          resource.extent.x, resource.extent.y);
                    }
                    else
                    {
                        ImGui::BulletText("%s '%s' (%llu bytes)", resourceTypeToString(resource.type),
                                          resource.debugName.c_str(),
                                          static_cast<unsigned long long>(resource.size));
                    }
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

        ImGui::End();
    }
}
