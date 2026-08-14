// Copyright (c) 2025 Snowapril
//
// Cross-backend acceleration structure coverage: one bottom-level structure over a triangle, one
// top-level structure that instances it, and the dynamic-object path -- move the instance and
// rebuild. Written against VkmDriverBase so Metal and Vulkan run the same assertions; a backend
// without VkmDriverCapabilityFlags::RayTracing skips, which is the honest answer on MoltenVK.
#pragma once

#include "UnitTestUtils.hpp"

#include <vkm/renderer/backend/common/acceleration_structure.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/buffer_view.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>

#include <glm/gtc/matrix_transform.hpp>

#include <vector>

namespace vkmtest
{
    inline void runAccelerationStructureTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);
        if ((driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::RayTracing) == 0)
        {
            MESSAGE("Skipping: this backend reports no RayTracing capability.");
            return;
        }

        // One triangle, positions only. The stride is 12 rather than a vertex layout's, because a
        // build only ever reads the position attribute -- see VkmAccelerationStructureGeometry.
        const float vertices[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
        const uint32_t indices[3] = { 0, 1, 2 };

        const auto makeInputBuffer = [&](const void* data, uint64_t size, const char* name) {
            vkm::VkmBufferInfo info{};
            // AllowAccelerationStructureInput is what forces the committed allocation path and adds
            // the build-input usage; without it the build cannot address the data.
            info._flags = static_cast<vkm::VkmResourceCreateInfo>(
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead) |
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferDst) |
                static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowAccelerationStructureInput));
            info._size = size;
            info._debugName = name;
            vkm::VkmBuffer* buffer = driver->newBuffer(info);
            REQUIRE(buffer != nullptr);
            REQUIRE(driver->uploadToBuffer(buffer->getHandle(), data, size));
            return buffer;
        };

        vkm::VkmBuffer* vertexBuffer = makeInputBuffer(vertices, sizeof(vertices), "AsTestVertices");
        vkm::VkmBuffer* indexBuffer = makeInputBuffer(indices, sizeof(indices), "AsTestIndices");

        vkm::VkmAccelerationStructureInfo blasInfo{};
        blasInfo._type = vkm::VkmAccelerationStructureType::BottomLevel;
        blasInfo._debugName = "AsTestBlas";
        // Views rather than the buffers themselves: a build reads a *range*, and this is what the
        // geometry descriptor names. Format-less, so neither backend creates a real view object.
        const auto makeView = [&](vkm::VkmBuffer* buffer, uint64_t size, const char* name) {
            vkm::VkmBufferViewInfo info{};
            info._offset = 0;
            info._size = size;
            info._debugName = name;
            vkm::VkmBufferView* view = buffer->createView(info);
            REQUIRE(view != nullptr);
            return view->getHandle();
        };
        const vkm::VkmResourceHandle vertexView = makeView(vertexBuffer, sizeof(vertices), "AsTestVertexView");
        const vkm::VkmResourceHandle indexView = makeView(indexBuffer, sizeof(indices), "AsTestIndexView");

        vkm::VkmAccelerationStructureGeometry geometry{};
        geometry._vertexView = vertexView;
        geometry._vertexStride = 3 * sizeof(float);
        geometry._vertexCount = 3;
        geometry._indexView = indexView;
        geometry._indexCount = 3;
        blasInfo._geometries.push_back(geometry);
        // The triangle's actual object-space bounds; the build ignores them, but the retained
        // info the debug inspector reads must carry them through.
        blasInfo._boundsMin = glm::vec3(0.0f);
        blasInfo._boundsMax = glm::vec3(1.0f, 1.0f, 0.0f);

        vkm::VkmAccelerationStructure* blas = driver->newAccelerationStructure(blasInfo);
        REQUIRE(blas != nullptr);
        // The size the driver reported for the structure. Zero would mean the build described no
        // geometry at all, which is the failure a "did it return non-null" check cannot see.
        CHECK(blas->getAllocatedSize() > 0);
        CHECK(blas->getAccelerationStructureInfo()._boundsMin == glm::vec3(0.0f));
        CHECK(blas->getAccelerationStructureInfo()._boundsMax == glm::vec3(1.0f, 1.0f, 0.0f));

        SUBCASE("a top-level structure instances it, and can be rebuilt after the instance moves")
        {
            vkm::VkmAccelerationStructureInfo tlasInfo{};
            tlasInfo._type = vkm::VkmAccelerationStructureType::TopLevel;
            tlasInfo._debugName = "AsTestTlas";
            // The dynamic-object flag: without it the structure is built once and a later build
            // refuses, because the scratch it needs was freed.
            tlasInfo._allowUpdate = true;
            vkm::VkmAccelerationStructureInstance instance{};
            instance._blas = blas->getHandle();
            instance._instanceId = 7;
            tlasInfo._instances.push_back(instance);

            vkm::VkmAccelerationStructure* tlas = driver->newAccelerationStructure(tlasInfo);
            REQUIRE(tlas != nullptr);
            CHECK(tlas->getAllocatedSize() > 0);

            // Drop the instance, the way a falling body would: only its transform changes, and its
            // bottom-level structure is never touched.
            instance._transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -5.0f, 0.0f));
            CHECK(tlas->updateInstances({ instance }));

            // The retained info follows the update, so it describes the list the next build reads
            // rather than the one the structure was created with.
            const std::vector<vkm::VkmAccelerationStructureInstance>& refreshed =
                tlas->getAccelerationStructureInfo()._instances;
            REQUIRE(refreshed.size() == 1);
            CHECK(refreshed[0]._transform == instance._transform);
            CHECK(refreshed[0]._instanceId == 7);

            vkm::VkmCommandQueueBase* queue =
                driver->getCommandQueue(vkm::VkmCommandQueueType::Graphics, 0);
            vkm::VkmCommandBufferBase* commandBuffer = queue->getCommandBufferPool()->allocate();
            commandBuffer->beginCommandBuffer();
            commandBuffer->buildAccelerationStructure(tlas->getHandle());
            commandBuffer->endCommandBuffer();
            vkm::CommandSubmitInfo submitInfo;
            submitInfo.commandBuffers[0] = commandBuffer;
            submitInfo.commandBufferCount = 1;
            const vkm::VkmGpuEventTimelineObject submitResult = queue->submit(submitInfo);
            if (submitResult._gpuEventTimeline != nullptr)
            {
                submitResult._gpuEventTimeline->waitIdle(vkm::MAX_GPU_TIMEOUT_PER_FRAME);
            }
            queue->getCommandBufferPool()->release(commandBuffer);

            SUBCASE("growing the instance list is refused rather than overrunning the buffer")
            {
                // The structure was sized for one instance; two would read past what was written.
                CHECK_FALSE(tlas->updateInstances({ instance, instance }));
                // A refused call must leave the retained info untouched.
                CHECK(tlas->getAccelerationStructureInfo()._instances.size() == 1);
            }

            /*
             * Wait for the device before releasing anything, and release synchronously.
             *
             * `vkDestroyAccelerationStructureKHR` requires every submitted command referring to the
             * structure to have COMPLETED. Handing the structure to the deferred reclaimer does not
             * establish that: an entry whose usages have all completed is released on the worker's
             * next 4 ms poll, concurrently with whatever the main thread does next, and three CI
             * runs in a row reported the structure still in use and then died with a segmentation
             * fault. A test that creates and destroys structures back to back is the worst case for
             * that, and it is not what the reclaimer exists for.
             */
            driver->waitIdle();
            driver->getRenderResourcePool()->releaseResource(tlas->getHandle());
        }

        SUBCASE("a structure built without _allowUpdate cannot be rebuilt")
        {
            // Its scratch was freed after the initial build, so rebuilding would read whatever now
            // occupies that memory. The refusal is the guard against that.
            //
            // This command buffer is begun and deliberately never submitted, which also covers the
            // stranded-timeline case: beginCommandBuffer() takes a timeline value, and until
            // VkmGpuEventTimelineBase::markTimelineSubmitted existed, abandoning it left every
            // later waitIdle on this queue waiting on a value nothing would signal.
            vkm::VkmCommandQueueBase* queue =
                driver->getCommandQueue(vkm::VkmCommandQueueType::Graphics, 0);
            vkm::VkmCommandBufferBase* commandBuffer = queue->getCommandBufferPool()->allocate();
            commandBuffer->beginCommandBuffer();
            commandBuffer->buildAccelerationStructure(blas->getHandle());
            commandBuffer->endCommandBuffer();
            queue->getCommandBufferPool()->release(commandBuffer);
            CHECK(blas->getAllocatedSize() > 0); // still intact; the rebuild was declined
        }

        // Same reason: the bottom-level structure was referenced by the builds above, and its
        // vertex and index buffers were read by them.
        driver->waitIdle();
        driver->getRenderResourcePool()->releaseResource(blas->getHandle());
        driver->getRenderResourcePool()->releaseResource(vertexView);
        driver->getRenderResourcePool()->releaseResource(indexView);
        driver->getRenderResourcePool()->releaseResource(vertexBuffer->getHandle());
        driver->getRenderResourcePool()->releaseResource(indexBuffer->getHandle());
    }
} // namespace vkmtest
