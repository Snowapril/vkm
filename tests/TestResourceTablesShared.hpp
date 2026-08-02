#ifndef TEST_RESOURCE_TABLES_SHARED_HPP
#define TEST_RESOURCE_TABLES_SHARED_HPP

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/staging_buffer.h>

#include <cstring>
#include <string>
#include <vector>

/*
* Backend-agnostic body for the PSO-declared descriptor sets, 2 (per-pass) and 3 (per-draw).
*
* Running the same shaders and the same expected values on every backend is the point. The three
* express these sets completely differently -- Vulkan allocates real descriptor sets, Metal binds
* discretely onto the argument table at indices vkm-compiler pinned, WebGPU builds bind groups --
* so agreement here is what shows one PSO declaration means the same thing on each.
*
* Two shapes are covered, and they fail differently:
*
*   - **Both sets at once.** The output is `pass.base + draw.base + threadId`, so a set 3 that
*     aliased onto set 2 -- the failure Metal invites, having no set index and separating the two
*     only by argument-table index bases -- yields a wrong number rather than a plausible one.
*   - **Set 3 without set 2.** A PSO may declare the per-draw set alone; a G-buffer pass wanting
*     only per-material textures does. Set 3 must still land at set index 3, which needs a
*     placeholder layout at index 2 -- without it the set slides down and the shader reads nothing.
*
* Writing `base + threadId` rather than a constant means a table bound at the wrong index, or
* buffers bound at swapped bindings, produces wrong values instead of plausible ones. The base
* values have no path into these shaders except through the sets under test. Validation layers are
* on throughout.
*/

namespace vkmtest
{
    constexpr uint32_t kTableElementCount = 64; // one threadgroup, matching [numthreads(64,1,1)]
    constexpr uint32_t kPerPassBaseValue = 1000;
    // Deliberately small and coprime-ish to the per-pass value: if the two sets were swapped or
    // aliased, the sum has to land somewhere the correct sum never does.
    constexpr uint32_t kPerDrawBaseValue = 7;

    // Mirrors TableConstants in resource_tables.hlsl (16 bytes: one uint plus its padding).
    struct TableConstants
    {
        uint32_t base;
        uint32_t _pad[3];
    };

    namespace detail
    {
        inline vkm::VkmBuffer* makeUniform(vkm::VkmDriverBase* driver, uint32_t base, const char* name)
        {
            // AllowShaderRead maps to VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT and AllowShaderWrite to
            // VK_BUFFER_USAGE_STORAGE_BUFFER_BIT (toVkBufferUsageFlags), so the flags pick the
            // usage each declared descriptor type needs.
            vkm::VkmBufferInfo info{};
            info._flags = vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowTransferDst;
            info._size = sizeof(TableConstants);
            info._debugName = name;
            vkm::VkmBuffer* buffer = driver->newBuffer(info);
            if (buffer != nullptr)
            {
                const TableConstants constants{base, {0, 0, 0}};
                driver->uploadToBuffer(buffer->getHandle(), &constants, sizeof(constants));
            }
            return buffer;
        }

        inline vkm::VkmBuffer* makeOutput(vkm::VkmDriverBase* driver, const char* name)
        {
            vkm::VkmBufferInfo info{};
            info._flags = vkm::VkmResourceCreateInfo::AllowShaderWrite | vkm::VkmResourceCreateInfo::AllowTransferSrc;
            info._size = sizeof(uint32_t) * kTableElementCount;
            info._debugName = name;
            return driver->newBuffer(info);
        }

        // Dispatches one threadgroup with `tables` bound, then reads the output buffer back.
        inline std::vector<uint32_t> dispatchAndRead(vkm::VkmDriverBase* driver,
                                                     vkm::VkmPipelineStateBase* pso,
                                                     const std::vector<vkm::VkmResourceTableBase*>& tables,
                                                     vkm::VkmResourceHandle output)
        {
            {
                vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
                auto* subGraph = renderGraph.beginComputeSubGraph("ResourceTableDispatch");
                subGraph->setComputeCallback([pso, tables](vkm::VkmCommandBufferBase* commandBuffer) {
                    commandBuffer->bindPipeline(pso);
                    for (vkm::VkmResourceTableBase* table : tables)
                    {
                        commandBuffer->bindResourceTable(table);
                    }
                    commandBuffer->dispatch(1);
                    commandBuffer->unbindPipeline();
                });
                renderGraph.compile();
                renderGraph.execute();
                renderGraph.ensureCompleted();
            }

            const uint64_t byteSize = sizeof(uint32_t) * kTableElementCount;
            vkm::VkmStagingBufferInfo stagingInfo{};
            stagingInfo._flags = vkm::VkmResourceCreateInfo::AllowTransferDst;
            stagingInfo._size = byteSize;
            stagingInfo._debugName = "ResourceTableReadback";
            vkm::VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
            REQUIRE(staging != nullptr);

            {
                const vkm::VkmResourceHandle destination = staging->getHandle();
                vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
                auto* subGraph = renderGraph.beginTransferSubGraph("ResourceTableReadback");
                subGraph->setTransferCallback([=](vkm::VkmCommandBufferBase* commandBuffer) {
                    commandBuffer->copyBuffer(output, destination, 0, 0, byteSize);
                });
                renderGraph.compile();
                renderGraph.execute();
                renderGraph.ensureCompleted();
            }

            std::vector<uint32_t> readback(kTableElementCount, 0);
            staging->invalidate(0, byteSize);
            std::memcpy(readback.data(), staging->map(), byteSize);
            driver->getRenderResourcePool()->releaseResource(staging->getHandle());
            return readback;
        }

        inline void checkSequence(const std::vector<uint32_t>& values, uint32_t expectedBase)
        {
            bool allMatch = true;
            for (uint32_t i = 0; i < kTableElementCount; ++i)
            {
                if (values[i] != expectedBase + i)
                {
                    allMatch = false;
                    // Report the first mismatch rather than 64 separate failures.
                    CHECK_MESSAGE(values[i] == expectedBase + i,
                                  "element " << i << " was " << values[i] << ", expected " << (expectedBase + i));
                    break;
                }
            }
            CHECK(allMatch);
        }

        inline vkm::VkmPipelineStateBase* loadPso(vkm::VkmPipelineStateManager& manager, const char* name)
        {
            std::string err;
            REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_RESOURCE_TABLE_PSO_DIR,
                                                                    TEST_RESOURCE_TABLE_SHADER_CACHE_DIR,
                                                                    vkm::VkmPipelineStateOrigin::User, &err), err);
            return manager.getPipelineState(name, vkm::VkmPipelineStateOrigin::User);
        }
    } // namespace detail

    // Sets 2 and 3 bound together against one pipeline that declares both.
    inline void runResourceTableTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmPipelineStateManager manager(driver);
        vkm::VkmPipelineStateBase* pso = detail::loadPso(manager, "resource_tables_pso[default]");
        REQUIRE(pso != nullptr);
        REQUIRE(pso->getDescriptor().perPassResources.size() == 2);
        REQUIRE(pso->getDescriptor().perDrawResources.size() == 1);

        vkm::VkmBuffer* passConstants = detail::makeUniform(driver, kPerPassBaseValue, "PerPassConstants");
        REQUIRE(passConstants != nullptr);
        vkm::VkmBuffer* drawConstants = detail::makeUniform(driver, kPerDrawBaseValue, "PerDrawConstants");
        REQUIRE(drawConstants != nullptr);
        vkm::VkmBuffer* output = detail::makeOutput(driver, "PerPassOutput");
        REQUIRE(output != nullptr);

        std::string err;
        vkm::VkmResourceTableBase* passTable = driver->newResourceTable(
            pso, vkm::VkmResourceSetKind::PerPass,
            {{ 0, passConstants->getHandle() }, { 1, output->getHandle() }}, &err);
        REQUIRE_MESSAGE(passTable != nullptr, err);
        vkm::VkmResourceTableBase* drawTable = driver->newResourceTable(
            pso, vkm::VkmResourceSetKind::PerDraw, {{ 0, drawConstants->getHandle() }}, &err);
        REQUIRE_MESSAGE(drawTable != nullptr, err);

        CHECK(passTable->getSetIndex() == vkm::kVkmPerPassSetIndex);
        CHECK(drawTable->getSetIndex() == vkm::kVkmPerDrawSetIndex);

        const std::vector<uint32_t> values =
            detail::dispatchAndRead(driver, pso, {passTable, drawTable}, output->getHandle());
        // Both sets contribute, so this number is only reachable when each landed at its own set.
        detail::checkSequence(values, kPerPassBaseValue + kPerDrawBaseValue);

        drawTable->destroy();
        delete drawTable;
        passTable->destroy();
        delete passTable;
        vkm::VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();
        resourcePool->releaseResource(output->getHandle());
        resourcePool->releaseResource(drawConstants->getHandle());
        resourcePool->releaseResource(passConstants->getHandle());
    }

    // A pipeline that declares set 3 and no set 2: the set must still land at index 3.
    inline void runPerDrawOnlyTableTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmPipelineStateManager manager(driver);
        vkm::VkmPipelineStateBase* pso = detail::loadPso(manager, "per_draw_only_pso[default]");
        REQUIRE(pso != nullptr);
        CHECK(pso->getDescriptor().perPassResources.empty());
        REQUIRE(pso->getDescriptor().perDrawResources.size() == 2);

        vkm::VkmBuffer* drawConstants = detail::makeUniform(driver, kPerDrawBaseValue, "PerDrawOnlyConstants");
        REQUIRE(drawConstants != nullptr);
        vkm::VkmBuffer* output = detail::makeOutput(driver, "PerDrawOnlyOutput");
        REQUIRE(output != nullptr);

        std::string err;
        vkm::VkmResourceTableBase* drawTable = driver->newResourceTable(
            pso, vkm::VkmResourceSetKind::PerDraw,
            {{ 0, drawConstants->getHandle() }, { 1, output->getHandle() }}, &err);
        REQUIRE_MESSAGE(drawTable != nullptr, err);
        CHECK(drawTable->getSetIndex() == vkm::kVkmPerDrawSetIndex);

        const std::vector<uint32_t> values =
            detail::dispatchAndRead(driver, pso, {drawTable}, output->getHandle());
        detail::checkSequence(values, kPerDrawBaseValue);

        drawTable->destroy();
        delete drawTable;
        vkm::VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();
        resourcePool->releaseResource(output->getHandle());
        resourcePool->releaseResource(drawConstants->getHandle());
    }

    inline void runResourceTableValidationTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmPipelineStateManager manager(driver);
        vkm::VkmPipelineStateBase* pso = detail::loadPso(manager, "resource_tables_pso[default]");
        REQUIRE(pso != nullptr);

        vkm::VkmBufferInfo bufferInfo{};
        bufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderRead | vkm::VkmResourceCreateInfo::AllowShaderWrite;
        bufferInfo._size = 256;
        bufferInfo._debugName = "ResourceTableValidationBuffer";
        vkm::VkmBuffer* buffer = driver->newBuffer(bufferInfo);
        REQUIRE(buffer != nullptr);

        SUBCASE("too few entries") {
            const std::vector<vkm::VkmTableResourceEntry> entries{ { 0, buffer->getHandle() } };
            std::string outError;
            CHECK(driver->newResourceTable(pso, vkm::VkmResourceSetKind::PerPass, entries, &outError) == nullptr);
            CHECK(outError.find("declares 2 per-pass resources but 1") != std::string::npos);
        }

        SUBCASE("a binding the pipeline never declared") {
            const std::vector<vkm::VkmTableResourceEntry> entries{
                { 0, buffer->getHandle() },
                { 9, buffer->getHandle() },
            };
            std::string outError;
            CHECK(driver->newResourceTable(pso, vkm::VkmResourceSetKind::PerPass, entries, &outError) == nullptr);
            CHECK(outError.find("binding 1") != std::string::npos);
        }

        SUBCASE("entries sized for the other set") {
            // Set 3 declares one binding on this PSO, so supplying set 2's two entries has to be
            // rejected against the per-draw declaration rather than accepted against set 2's.
            const std::vector<vkm::VkmTableResourceEntry> entries{
                { 0, buffer->getHandle() },
                { 1, buffer->getHandle() },
            };
            std::string outError;
            CHECK(driver->newResourceTable(pso, vkm::VkmResourceSetKind::PerDraw, entries, &outError) == nullptr);
            CHECK(outError.find("declares 1 per-draw resources but 2") != std::string::npos);
        }

        SUBCASE("a set the pipeline does not declare at all") {
            vkm::VkmPipelineStateBase* perDrawOnly = detail::loadPso(manager, "per_draw_only_pso[default]");
            REQUIRE(perDrawOnly != nullptr);
            const std::vector<vkm::VkmTableResourceEntry> entries{ { 0, buffer->getHandle() } };
            std::string outError;
            CHECK(driver->newResourceTable(perDrawOnly, vkm::VkmResourceSetKind::PerPass, entries, &outError) == nullptr);
            CHECK(outError.find("no per-pass resources") != std::string::npos);
        }

        driver->getRenderResourcePool()->releaseResource(buffer->getHandle());
    }
} // namespace vkmtest

#endif // TEST_RESOURCE_TABLES_SHARED_HPP
