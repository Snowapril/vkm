#ifndef TEST_BUFFER_HOST_WRITE_SHARED_HPP
#define TEST_BUFFER_HOST_WRITE_SHARED_HPP

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/staging_buffer.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace vkmtest
{
    inline constexpr uint64_t kHostWriteBufferSize = 1024;

    // Byte pattern parameterized by a seed, so two uploads can be told apart. A silently
    // no-op second upload would leave the first pattern behind and fail the comparison.
    inline std::vector<uint8_t> makeBufferPattern(uint8_t seed, size_t size = kHostWriteBufferSize)
    {
        std::vector<uint8_t> pattern(size);
        for (size_t i = 0; i < size; ++i)
        {
            pattern[i] = static_cast<uint8_t>((i * 7 + seed * 31 + 3) & 0xFF);
        }
        return pattern;
    }

    /*
    * @brief Copies `size` bytes out of a device buffer through a readback staging buffer.
    * @details The engine has no readbackBuffer() counterpart to readbackTexture(), so this is
    * the same copyBuffer + one-off submit + wait + map sequence TestMetalBindlessTriangle
    * already uses -- all of it through engine abstractions, per tests/CLAUDE.md.
    */
    inline std::vector<uint8_t> readBackBuffer(vkm::VkmDriverBase* driver, vkm::VkmResourceHandle buffer, uint64_t size)
    {
        vkm::VkmStagingBufferInfo readbackInfo{};
        readbackInfo._flags = vkm::VkmResourceCreateInfo::AllowTransferDst;
        readbackInfo._size = size;
        readbackInfo._debugName = "HostWriteReadbackStaging";
        vkm::VkmStagingBuffer* readbackStaging = driver->newStagingBuffer(readbackInfo);
        REQUIRE(readbackStaging != nullptr);

        vkm::VkmCommandQueueBase* queue = driver->getCommandQueue(vkm::VkmCommandQueueType::Graphics, 0);
        vkm::VkmCommandBufferBase* commandBuffer = queue->getCommandBufferPool()->allocate();
        commandBuffer->beginCommandBuffer();
        commandBuffer->copyBuffer(buffer, readbackStaging->getHandle(), 0, 0, size);
        commandBuffer->endCommandBuffer();

        vkm::CommandSubmitInfo submitInfo;
        submitInfo.commandBuffers[0] = commandBuffer;
        submitInfo.commandBufferCount = 1;
        vkm::VkmGpuEventTimelineObject submitResult = queue->submit(submitInfo);
        REQUIRE(submitResult._gpuEventTimeline != nullptr);
        submitResult._gpuEventTimeline->waitIdle(vkm::MAX_GPU_TIMEOUT_PER_FRAME);
        queue->getCommandBufferPool()->release(commandBuffer);

        readbackStaging->invalidate(0, size);
        const uint8_t* mapped = static_cast<const uint8_t*>(readbackStaging->map());
        REQUIRE(mapped != nullptr);
        std::vector<uint8_t> result(mapped, mapped + size);
        readbackStaging->unmap();

        driver->getRenderResourcePool()->releaseResource(readbackStaging->getHandle());
        return result;
    }

    inline vkm::VkmBuffer* createTestBuffer(vkm::VkmDriverBase* driver, vkm::VkmMemoryAccessHint accessHint,
                                            vkm::VkmMemoryPlacementHint placementHint = vkm::VkmMemoryPlacementHint::Auto)
    {
        vkm::VkmBufferInfo bufferInfo{};
        bufferInfo._flags = vkm::VkmResourceCreateInfo::AllowTransferSrc | vkm::VkmResourceCreateInfo::AllowTransferDst;
        bufferInfo._size = kHostWriteBufferSize;
        bufferInfo._accessHint = accessHint;
        bufferInfo._placementHint = placementHint;
        bufferInfo._debugName = "HostWriteTestBuffer";
        return driver->newBuffer(bufferInfo);
    }

    /*
    * @brief Backend-agnostic body for the host-write buffer upload path.
    * @details Runs with validation layers on, so the assertions are only half of what is being
    * tested -- a buffer allocated in memory that does not match the usage it was created with,
    * or a missing flush, surfaces as a validation error instead.
    */
    inline void runBufferHostWriteTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);

        vkm::VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();
        vkm::VkmBuffer* buffer = createTestBuffer(driver, vkm::VkmMemoryAccessHint::HostWrite);
        REQUIRE(buffer != nullptr);
        const vkm::VkmResourceHandle bufferHandle = buffer->getHandle();

        SUBCASE("a HostWrite buffer is host-writable and maps, a DeviceLocal one is neither")
        {
            // Guards against the whole feature silently degrading to staging: a HostWrite
            // request must actually produce host-visible memory on the two backends that drive
            // this body, not merely be eligible in principle.
            CHECK(buffer->isHostWritable());
            CHECK(buffer->map() != nullptr);

            vkm::VkmBuffer* deviceLocal = createTestBuffer(driver, vkm::VkmMemoryAccessHint::DeviceLocal);
            REQUIRE(deviceLocal != nullptr);
            CHECK_FALSE(deviceLocal->isHostWritable());
            CHECK(deviceLocal->map() == nullptr);
            resourcePool->releaseResource(deviceLocal->getHandle());
        }

        /*
        * The assertion that matters: whichever route the bytes take, the result must be
        * indistinguishable. The staging pass deliberately writes a *different* pattern, so a
        * host write that silently did nothing would leave the staging pattern behind and fail
        * here rather than pass by coincidence.
        */
        SUBCASE("the staging and host-write paths produce identical results")
        {
            const std::vector<uint8_t> staged = makeBufferPattern(1);
            REQUIRE(driver->uploadToBuffer(bufferHandle, staged.data(), staged.size(), 0,
                                           vkm::VkmResourceUploadMode::ForceStaging));
            CHECK(readBackBuffer(driver, bufferHandle, staged.size()) == staged);

            const std::vector<uint8_t> hostWritten = makeBufferPattern(2);
            REQUIRE(driver->uploadToBuffer(bufferHandle, hostWritten.data(), hostWritten.size(), 0,
                                           vkm::VkmResourceUploadMode::ForceHostCopy));
            CHECK(readBackBuffer(driver, bufferHandle, hostWritten.size()) == hostWritten);

            // Auto is what every real caller passes; on a host-writable buffer it must take the
            // direct write, which this proves only in the sense that the bytes land correctly.
            const std::vector<uint8_t> automatic = makeBufferPattern(3);
            REQUIRE(driver->uploadToBuffer(bufferHandle, automatic.data(), automatic.size(), 0,
                                           vkm::VkmResourceUploadMode::Auto));
            CHECK(readBackBuffer(driver, bufferHandle, automatic.size()) == automatic);
        }

        SUBCASE("a non-zero dstOffset writes only the range it was given")
        {
            const std::vector<uint8_t> base = makeBufferPattern(4);
            REQUIRE(driver->uploadToBuffer(bufferHandle, base.data(), base.size()));

            constexpr uint64_t kOffset = 256;
            constexpr uint64_t kPartialSize = 128;
            const std::vector<uint8_t> partial = makeBufferPattern(5, kPartialSize);
            REQUIRE(driver->uploadToBuffer(bufferHandle, partial.data(), partial.size(), kOffset));

            std::vector<uint8_t> expected = base;
            std::copy(partial.begin(), partial.end(), expected.begin() + kOffset);
            CHECK(readBackBuffer(driver, bufferHandle, expected.size()) == expected);
        }

        SUBCASE("a range past the end of the buffer is rejected on either path")
        {
            const std::vector<uint8_t> pattern = makeBufferPattern(6);
            CHECK_FALSE(driver->uploadToBuffer(bufferHandle, pattern.data(), pattern.size(), 1));
            CHECK_FALSE(driver->uploadToBuffer(bufferHandle, pattern.data(), pattern.size(), 1,
                                               vkm::VkmResourceUploadMode::ForceStaging));
        }

        SUBCASE("ForceHostCopy on a DeviceLocal buffer warns and falls back to staging")
        {
            vkm::VkmBuffer* deviceLocal = createTestBuffer(driver, vkm::VkmMemoryAccessHint::DeviceLocal);
            REQUIRE(deviceLocal != nullptr);

            const std::vector<uint8_t> pattern = makeBufferPattern(7);
            CHECK(driver->uploadToBuffer(deviceLocal->getHandle(), pattern.data(), pattern.size(), 0,
                                         vkm::VkmResourceUploadMode::ForceHostCopy));
            CHECK(readBackBuffer(driver, deviceLocal->getHandle(), pattern.size()) == pattern);
            resourcePool->releaseResource(deviceLocal->getHandle());
        }

        resourcePool->releaseResource(bufferHandle);
    }

    /*
    * @brief Backend-agnostic body for getGPUVirtualAddress().
    * @details The capability flag is the reference: every buffer must report an address when
    * the device offers one, and 0 when it does not. Asserting against the flag rather than the
    * backend keeps this correct on a Vulkan driver without bufferDeviceAddress.
    */
    inline void runBufferGpuAddressTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);

        const bool deviceSupportsAddresses =
            (driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::BufferDeviceAddress) != 0;
        CAPTURE(deviceSupportsAddresses);

        vkm::VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();

        // Committed, pooled and host-writable buffers take three different allocation paths;
        // the pooled one is the case where the address is the pool block's plus an offset.
        vkm::VkmBuffer* committed =
            createTestBuffer(driver, vkm::VkmMemoryAccessHint::DeviceLocal, vkm::VkmMemoryPlacementHint::Committed);
        vkm::VkmBuffer* pooled =
            createTestBuffer(driver, vkm::VkmMemoryAccessHint::DeviceLocal, vkm::VkmMemoryPlacementHint::Heap);
        vkm::VkmBuffer* hostWrite = createTestBuffer(driver, vkm::VkmMemoryAccessHint::HostWrite);
        REQUIRE(committed != nullptr);
        REQUIRE(pooled != nullptr);
        REQUIRE(hostWrite != nullptr);

        CHECK((committed->getGPUVirtualAddress() != 0) == deviceSupportsAddresses);
        CHECK((pooled->getGPUVirtualAddress() != 0) == deviceSupportsAddresses);
        CHECK((hostWrite->getGPUVirtualAddress() != 0) == deviceSupportsAddresses);

        // Two distinct allocations cannot share an address. This is what would catch a pooled
        // buffer reporting the block's base instead of its own sub-range.
        if (deviceSupportsAddresses)
        {
            CHECK(committed->getGPUVirtualAddress() != pooled->getGPUVirtualAddress());
            CHECK(committed->getGPUVirtualAddress() != hostWrite->getGPUVirtualAddress());
        }

        vkm::VkmStagingBufferInfo stagingInfo{};
        stagingInfo._flags = vkm::VkmResourceCreateInfo::AllowTransferSrc;
        stagingInfo._size = kHostWriteBufferSize;
        stagingInfo._debugName = "GpuAddressTestStaging";
        vkm::VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
        REQUIRE(staging != nullptr);
        CHECK((staging->getGPUVirtualAddress() != 0) == deviceSupportsAddresses);

        resourcePool->releaseResource(staging->getHandle());
        resourcePool->releaseResource(hostWrite->getHandle());
        resourcePool->releaseResource(pooled->getHandle());
        resourcePool->releaseResource(committed->getHandle());
    }
} // namespace vkmtest

#endif // TEST_BUFFER_HOST_WRITE_SHARED_HPP
