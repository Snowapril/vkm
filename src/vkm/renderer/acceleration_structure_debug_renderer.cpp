// Copyright (c) 2026 Snowapril

#include <vkm/renderer/acceleration_structure_debug_renderer.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/staging_buffer.h>

#include <string>

namespace vkm
{
    namespace
    {
        // 12 edges, two endpoints each; the vertex shader turns SV_VertexID into a corner.
        constexpr uint32_t kVertexCount = 24;

        bool fail(std::string* outError, const std::string& message)
        {
            VKM_DEBUG_ERROR(message.c_str());
            if (outError != nullptr)
            {
                *outError = message;
            }
            return false;
        }
    } // namespace

    bool VkmAccelerationStructureDebugRenderer::initialize(
        VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager, std::string* outError)
    {
        VKM_ASSERT(driver != nullptr, "VkmAccelerationStructureDebugRenderer requires a driver");
        VKM_ASSERT(pipelineStateManager != nullptr,
                   "VkmAccelerationStructureDebugRenderer requires a pipeline state manager");

        _driver = driver;
        _pipeline = pipelineStateManager->getPipelineState("as_wireframe_pso",
                                                           VkmPipelineStateOrigin::Engine);
        if (_pipeline == nullptr)
        {
            return fail(outError, "as_wireframe_pso is not loaded");
        }

        const uint64_t byteSize = static_cast<uint64_t>(kVkmAsDebugMaxBoxes) * sizeof(VkmAsDebugBox);

        VkmBufferInfo bufferInfo{};
        bufferInfo._flags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst));
        bufferInfo._size = byteSize;
        bufferInfo._debugName = "AsDebugBoxes";
        VkmBuffer* boxBuffer = driver->newBuffer(bufferInfo);
        if (boxBuffer == nullptr)
        {
            return fail(outError, "Failed to create the acceleration structure debug box buffer");
        }
        _boxBuffer = boxBuffer->getHandle();

        for (uint32_t frame = 0; frame < FRAME_BUFFER_COUNT; ++frame)
        {
            VkmStagingBufferInfo stagingInfo{};
            stagingInfo._flags = VkmResourceCreateInfo::AllowTransferSrc;
            stagingInfo._size = byteSize;
            stagingInfo._debugName = "AsDebugBoxesStaging";
            VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
            if (staging == nullptr)
            {
                return fail(outError,
                            "Failed to create an acceleration structure debug staging buffer");
            }
            _staging[frame] = staging->getHandle();
            _stagingBuffers[frame] = staging;
        }

        std::string tableError;
        _table = driver->newResourceTable(_pipeline, VkmResourceSetKind::PerPass,
                                          {{ 0, _boxBuffer }}, &tableError);
        if (_table == nullptr)
        {
            return fail(outError,
                        "Failed to build the acceleration structure debug table: " + tableError);
        }
        return true;
    }

    void VkmAccelerationStructureDebugRenderer::releaseResources()
    {
        if (_driver == nullptr)
        {
            return;
        }
        delete _table;
        _table = nullptr;

        VkmDeferredResourceReclaimer* reclaimer = _driver->getDeferredReclaimer();
        if (reclaimer != nullptr && _boxBuffer != VKM_INVALID_RESOURCE_HANDLE)
        {
            reclaimer->requestRelease(_boxBuffer);
        }
        _boxBuffer = VKM_INVALID_RESOURCE_HANDLE;

        for (uint32_t frame = 0; frame < FRAME_BUFFER_COUNT; ++frame)
        {
            // Gated on the pointer, which is null exactly when this slot was never created --
            // the two are assigned together, and a null pointer cannot be mistaken for a live
            // resource the way handle id 0 can.
            if (reclaimer != nullptr && _stagingBuffers[frame] != nullptr)
            {
                reclaimer->requestRelease(_staging[frame]);
            }
            _staging[frame] = VKM_INVALID_RESOURCE_HANDLE;
            _stagingBuffers[frame] = nullptr;
        }
        _pipeline = nullptr;
        _boxes.clear();
    }

    void VkmAccelerationStructureDebugRenderer::record(VkmRenderGraph* renderGraph,
                                                       VkmResourceHandle target,
                                                       const glm::uvec2& extent, uint32_t frameIndex)
    {
        if (!_enabled || _table == nullptr || renderGraph == nullptr || !target.isValid())
        {
            return;
        }
        VKM_ASSERT(frameIndex < FRAME_BUFFER_COUNT,
                   "VkmAccelerationStructureDebugRenderer::record got an out-of-range frame slot");

        const std::vector<VkmAccelerationStructureInstanceBox> instances =
            vkmCollectInstanceBoxes(vkmCollectAccelerationStructures(_driver->getRenderResourcePool()));
        if (!vkmBuildAsDebugBoxes(instances, _selected, &_boxes) && !_overflowLogged)
        {
            _overflowLogged = true;
            VKM_DEBUG_WARN(("The acceleration structure debug overlay draws only the first " +
                            std::to_string(kVkmAsDebugMaxBoxes) + " of " +
                            std::to_string(instances.size()) +
                            " instances; the rest are not outlined")
                               .c_str());
        }
        if (_boxes.empty())
        {
            return;
        }

        const uint32_t boxCount = static_cast<uint32_t>(_boxes.size());
        const uint64_t byteSize = static_cast<uint64_t>(boxCount) * sizeof(VkmAsDebugBox);

        // The boxes change every frame while the table binding them stays immutable, which is how
        // a per-pass table is meant to be used. One staging region per frame slot, for the same
        // reason VkmScene keeps one.
        //
        // The host write happens here rather than in the callback so the callback captures no
        // pointer into `_boxes`. It is safe this early because this slot's previous submit was
        // waited on by VkmRenderGraph::ensureCompleted() at the top of the frame, which is the
        // same precondition that lets the slot's region be reused at all.
        _stagingBuffers[frameIndex]->writeDirect(0, _boxes.data(), byteSize);

        VkmRenderTransferSubGraph* uploadSubGraph =
            renderGraph->beginTransferSubGraph("EngineAsDebugUpload");
        uploadSubGraph->addReferencedResource(_boxBuffer, VkmResourceAccess::TransferWrite);
        uploadSubGraph->addReferencedResource(_staging[frameIndex], VkmResourceAccess::TransferRead);
        const VkmResourceHandle stagingHandle = _staging[frameIndex];
        const VkmResourceHandle boxBuffer = _boxBuffer;
        uploadSubGraph->setTransferCallback(
            [stagingHandle, boxBuffer, byteSize](VkmCommandBufferBase* commandBuffer) {
                commandBuffer->copyBuffer(stagingHandle, boxBuffer, 0, 0, byteSize);
            });

        VkmFrameBufferDescriptor frameBufferDesc;
        frameBufferDesc._renderPass._colorAttachmentCount = 1;
        frameBufferDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        // Load, not Clear: this draws over whatever the app rendered.
        frameBufferDesc._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::Load;
        frameBufferDesc._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
        frameBufferDesc._width = extent.x;
        frameBufferDesc._height = extent.y;
        frameBufferDesc._colorAttachments[0] = target;

        VkmRenderGraphicsSubGraph* drawSubGraph =
            renderGraph->beginGraphicsSubGraph(frameBufferDesc, "EngineAsDebugWireframe");
        drawSubGraph->addReferencedResource(target, VkmResourceAccess::ColorAttachmentWrite);
        std::vector<VkmResourceAccessDeclaration> bound;
        _table->collectReferencedResources(&bound);
        drawSubGraph->addReferencedResources(bound);

        VkmPipelineStateBase* pipeline = _pipeline;
        VkmResourceTableBase* table = _table;
        drawSubGraph->setRenderCallback([pipeline, table, boxCount](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pipeline);
            commandBuffer->bindResourceTable(table);
            commandBuffer->draw(kVertexCount, boxCount, 0, 0);
        });
    }
} // namespace vkm
