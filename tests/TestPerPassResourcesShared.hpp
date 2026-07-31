#ifndef TEST_PER_PASS_RESOURCES_SHARED_HPP
#define TEST_PER_PASS_RESOURCES_SHARED_HPP

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/per_pass_resource_table.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/staging_buffer.h>

#include <cstring>
#include <string>
#include <vector>

/*
* Backend-agnostic body for descriptor set 2: a compute shader whose only resources are a set-2
* uniform buffer and a set-2 storage buffer, dispatched with a VkmPerPassResourceTable bound.
*
* Running the same shader and the same expected values on both backends is the point. Vulkan and
* Metal express set 2 completely differently -- Vulkan allocates a real descriptor set, Metal binds
* discretely onto the argument table at indices vkm-compiler pinned -- so agreement here is what
* shows the PSO declaration means the same thing on each.
*
* The shader writes `base + threadId` rather than a constant, so a table bound at the wrong index,
* or buffers bound at swapped bindings, produces wrong values instead of plausible ones. The value
* 1000 has no path into that shader except through set 2. Validation layers are on throughout.
*/

namespace vkmtest
{
    constexpr uint32_t kPerPassElementCount = 64; // one threadgroup, matching [numthreads(64,1,1)]
    constexpr uint32_t kPerPassBaseValue = 1000;

    // Mirrors PassConstants in per_pass_resources.hlsl (16 bytes: one uint plus its padding).
    struct PerPassConstants
    {
        uint32_t base;
        uint32_t _pad[3];
    };

    inline void runPerPassResourceTest(vkm::VkmDriverBase* driver)
    {

    vkm::VkmPipelineStateManager manager(driver);
    std::string err;
    REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_PER_PASS_PSO_DIR, TEST_PER_PASS_SHADER_CACHE_DIR,
                                                            vkm::VkmPipelineStateOrigin::User, &err), err);
    vkm::VkmPipelineStateBase* pso =
        manager.getPipelineState("per_pass_resources_pso[default]", vkm::VkmPipelineStateOrigin::User);
    REQUIRE(pso != nullptr);
    REQUIRE(pso->getDescriptor().perPassResources.size() == 2);

    // AllowShaderRead maps to VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT and AllowShaderWrite to
    // VK_BUFFER_USAGE_STORAGE_BUFFER_BIT (toVkBufferUsageFlags), so the flags pick the usage the
    // set-2 descriptor type needs.
    vkm::VkmBufferInfo constantsInfo{};
    constantsInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowTransferDst;
    constantsInfo._size = sizeof(PerPassConstants);
    constantsInfo._debugName = "PerPassConstants";
    vkm::VkmBuffer* constants = driver->newBuffer(constantsInfo);
    REQUIRE(constants != nullptr);

    vkm::VkmBufferInfo outputInfo{};
    outputInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderWrite | vkm::VkmResourceCreateInfo::AllowTransferSrc;
    outputInfo._size = sizeof(uint32_t) * kPerPassElementCount;
    outputInfo._debugName = "PerPassOutput";
    vkm::VkmBuffer* output = driver->newBuffer(outputInfo);
    REQUIRE(output != nullptr);

    const PerPassConstants passConstants{kPerPassBaseValue, {0, 0, 0}};
    REQUIRE(driver->uploadToBuffer(constants->getHandle(), &passConstants, sizeof(passConstants)));

    const std::vector<vkm::VkmPerPassResourceEntry> entries{
        { 0, constants->getHandle() },
        { 1, output->getHandle() },
    };
    vkm::VkmPerPassResourceTableBase* table = driver->newPerPassResourceTable(pso, entries, &err);
    REQUIRE_MESSAGE(table != nullptr, err);

    {
        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* subGraph = renderGraph.beginComputeSubGraph("PerPassDispatch");
        subGraph->setComputeCallback([pso, table](vkm::VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pso);
            commandBuffer->bindPerPassResources(table);
            commandBuffer->dispatch(1);
            commandBuffer->unbindPipeline();
        });
        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();
    }

    vkm::VkmStagingBufferInfo stagingInfo{};
    stagingInfo._flags = vkm::VkmResourceCreateInfo::AllowTransferDst;
    stagingInfo._size = outputInfo._size;
    stagingInfo._debugName = "PerPassReadback";
    vkm::VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
    REQUIRE(staging != nullptr);

    {
        const vkm::VkmResourceHandle source = output->getHandle();
        const vkm::VkmResourceHandle destination = staging->getHandle();
        const uint64_t byteSize = outputInfo._size;
        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* subGraph = renderGraph.beginTransferSubGraph("PerPassReadback");
        subGraph->setTransferCallback([=](vkm::VkmCommandBufferBase* commandBuffer) {
            commandBuffer->copyBuffer(source, destination, 0, 0, byteSize);
        });
        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();
    }

    std::vector<uint32_t> readback(kPerPassElementCount, 0);
    staging->invalidate(0, outputInfo._size);
    std::memcpy(readback.data(), staging->map(), outputInfo._size);

    bool allMatch = true;
    for (uint32_t i = 0; i < kPerPassElementCount; ++i)
    {
        if (readback[i] != kPerPassBaseValue + i)
        {
            allMatch = false;
            // Report the first mismatch rather than 64 separate failures.
            CHECK_MESSAGE(readback[i] == kPerPassBaseValue + i,
                          "element " << i << " was " << readback[i] << ", expected " << (kPerPassBaseValue + i));
            break;
        }
    }
    CHECK(allMatch);

    table->destroy();
    delete table;
    vkm::VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();
    resourcePool->releaseResource(staging->getHandle());
    resourcePool->releaseResource(output->getHandle());
    resourcePool->releaseResource(constants->getHandle());
}

    inline void runPerPassValidationTest(vkm::VkmDriverBase* driver)
    {

    vkm::VkmPipelineStateManager manager(driver);
    std::string err;
    REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_PER_PASS_PSO_DIR, TEST_PER_PASS_SHADER_CACHE_DIR,
                                                            vkm::VkmPipelineStateOrigin::User, &err), err);
    vkm::VkmPipelineStateBase* pso =
        manager.getPipelineState("per_pass_resources_pso[default]", vkm::VkmPipelineStateOrigin::User);
    REQUIRE(pso != nullptr);

    vkm::VkmBufferInfo bufferInfo{};
    bufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowShaderWrite;
    bufferInfo._size = 256;
    bufferInfo._debugName = "PerPassValidationBuffer";
    vkm::VkmBuffer* buffer = driver->newBuffer(bufferInfo);
    REQUIRE(buffer != nullptr);

    SUBCASE("too few entries") {
        const std::vector<vkm::VkmPerPassResourceEntry> entries{ { 0, buffer->getHandle() } };
        std::string outError;
        CHECK(driver->newPerPassResourceTable(pso, entries, &outError) == nullptr);
        CHECK(outError.find("declares 2 per-pass resources but 1") != std::string::npos);
    }

    SUBCASE("a binding the pipeline never declared") {
        const std::vector<vkm::VkmPerPassResourceEntry> entries{
            { 0, buffer->getHandle() },
            { 9, buffer->getHandle() },
        };
        std::string outError;
        CHECK(driver->newPerPassResourceTable(pso, entries, &outError) == nullptr);
        CHECK(outError.find("binding 1") != std::string::npos);
    }

    driver->getRenderResourcePool()->releaseResource(buffer->getHandle());
}

}

#endif // TEST_PER_PASS_RESOURCES_SHARED_HPP
