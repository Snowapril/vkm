// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/acceleration_structure_debug.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmPipelineStateBase;
    class VkmPipelineStateManager;
    class VkmRenderGraph;
    class VkmResourceTableBase;
    class VkmStagingBuffer;

    /*
    * @brief The acceleration structure inspector's 3D view: every top-level instance outlined as
    * a wireframe box in the scene itself.
    * @details Engine-owned and recorded from `VkmEngine::render()` after the app has drawn, so no
    * sample has to opt in. Draws over the colour target it is given with a Load action and no
    * depth test, which is what lets it show an instance buried inside geometry.
    *
    * The box list comes from the resource pool every frame -- `VkmAccelerationStructure::
    * updateInstances` refreshes the info this reads, so a rebuilt top-level structure is followed
    * without any notification. It rides a uniform buffer rather than a storage buffer because a
    * set-2 storage buffer is compute-visible only (see VkmTableResourceType::StorageBuffer), which
    * caps the overlay at `kVkmAsDebugMaxBoxes`.
    */
    class VkmAccelerationStructureDebugRenderer
    {
    public:
        /*
        * @brief Resolves the pipeline and creates the box buffer and its per-frame staging buffers.
        * @details Must follow the engine pipeline directory's load, the pipeline being looked up
        * by name. Safe to skip entirely: a renderer that was never initialized records nothing.
        * @param driver Driver that owns the resources and whose pool is enumerated.
        * @param pipelineStateManager Manager holding the engine-origin pipeline states.
        * @param outError Receives the reason on failure. May be null.
        * @return False when the pipeline is missing or a resource fails to create.
        */
        bool initialize(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager,
                        std::string* outError);

        /*
        * @brief Destroys the table and hands every buffer to the deferred reclaimer.
        * @details Safe on a renderer that was never initialized. After it the renderer records
        * nothing.
        */
        void releaseResources();

        /*
        * @brief Records this frame's box upload and the line draw.
        * @details A no-op unless enabled and initialized, and unless the pool holds a top-level
        * instance with recorded bounds. Records a transfer subgraph and then a graphics subgraph,
        * in that order, so the copy is published before the draw reads it.
        * @param renderGraph Graph to record into.
        * @param target Colour target to draw over. Loaded, not cleared.
        * @param extent Target extent in pixels, for the framebuffer descriptor.
        * @param frameIndex Frame slot, selecting this frame's staging buffer.
        */
        void record(VkmRenderGraph* renderGraph, VkmResourceHandle target, const glm::uvec2& extent,
                    uint32_t frameIndex);

        void setEnabled(bool enabled) { _enabled = enabled; }
        bool isEnabled() const { return _enabled; }

        // The structure drawn highlighted. A top-level handle marks all of its instances, a
        // bottom-level one marks every instance of it.
        void setSelected(VkmResourceHandle selected) { _selected = selected; }

    private:
        VkmDriverBase* _driver = nullptr;
        VkmPipelineStateBase* _pipeline = nullptr;
        VkmResourceTableBase* _table = nullptr;
        VkmResourceHandle _boxBuffer = VKM_INVALID_RESOURCE_HANDLE;
        // Named explicitly rather than left to `{}`, which would give each entry id 0 -- a value
        // isValid() accepts and that names whatever really owns slot 0.
        VkmResourceHandle _staging[FRAME_BUFFER_COUNT] = {
            VKM_INVALID_RESOURCE_HANDLE, VKM_INVALID_RESOURCE_HANDLE, VKM_INVALID_RESOURCE_HANDLE };
        static_assert(FRAME_BUFFER_COUNT == 3, "_staging's initializer names one handle per slot");
        VkmStagingBuffer* _stagingBuffers[FRAME_BUFFER_COUNT] = {};

        bool _enabled = false;
        VkmResourceHandle _selected = VKM_INVALID_RESOURCE_HANDLE;
        // Reused across frames so a per-frame collect does not reallocate.
        std::vector<VkmAsDebugBox> _boxes;
        // One-shot, so a scene that exceeds the capacity does not log every frame.
        bool _overflowLogged = false;
    };
} // namespace vkm
