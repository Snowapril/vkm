// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmImGuiRendererBase;

    /*
    * @brief ImGui browser over every texture currently live in the driver's resource pool --
    * bindless-registered assets, render graph attachments, capture snapshots and everything
    * else. Drawn as a tab of VkmRenderGraphInspector rather than its own window.
    *
    * Two preview paths. A plain single-layer 2D color texture is shown live and zero-copy
    * through VkmImGuiRendererBase::getTextureID(); a cube or array texture is shown one face
    * at a time by reading the selected layer back to the CPU and re-uploading it into a
    * reusable preview texture, because the ImGui pipeline samples a 2D texture only.
    */
    class VkmTextureBrowser
    {
    public:
        void draw(VkmDriverBase* driver, VkmImGuiRendererBase* imGuiRenderer);

        // Releases the layer-preview texture. Called from VkmEngine::destroy() alongside the
        // capture's own releaseResources().
        void releaseResources(VkmDriverBase* driver);

        /*
        * @brief Textures this browser emitted an ImGui::Image() for during the last draw().
        * The engine references them on the ImGui overlay subgraph so their GPU usage is
        * tracked while those draws are in flight -- the same reason the capture's snapshot
        * handles are re-referenced there.
        */
        const std::vector<VkmResourceHandle>& getTexturesSampledLastDraw() const { return _sampledTextures; }

    private:
        // Fills the preview texture with `layer` of `source`, recreating it when the source's
        // format or extent changed. Returns false when the readback path cannot serve this
        // texture (unsupported format or backend).
        bool refreshLayerPreview(VkmDriverBase* driver, VkmResourceHandle source, uint32_t layer);

        VkmResourceHandle _selected = VKM_INVALID_RESOURCE_HANDLE;
        // Reusable destination for the cube/array face preview, in the source's own format so
        // no channel swizzling is needed (readbackTexture returns native channel order).
        VkmResourceHandle _layerPreview = VKM_INVALID_RESOURCE_HANDLE;
        VkmResourceHandle _layerPreviewSource = VKM_INVALID_RESOURCE_HANDLE;
        VkmFormat _layerPreviewFormat = VkmFormat::Undefined;
        glm::uvec3 _layerPreviewExtent{0, 0, 0};
        std::string _layerPreviewName;
        uint32_t _selectedLayer = 0;
        float _previewScale = 1.0f;
        char _nameFilter[128] = {};
        std::vector<VkmResourceHandle> _sampledTextures;
    };
} // namespace vkm
