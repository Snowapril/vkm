#include "UnitTestUtils.hpp"

#include <vkm/base/platform.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/gpu_offset_allocator.h>
#include <vkm/renderer/backend/common/render_resource.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/texture_view.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/engine.h>
#include <vkm/renderer/memory_report.h>
#include <vkm/platform/common/app_delegate.h>

#include <algorithm>
#include <functional>

#ifdef VKM_USE_VULKAN_API
#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_staging_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_sampler.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture_view.h>
#include <vkm/renderer/backend/vulkan/vulkan_buffer_view.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_queue.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <glm/vec2.hpp>
#endif

#ifdef VKM_USE_WEBGPU_API
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_command_queue.h>
#endif

// ---------------------------------------------------------------------------
// Pure-logic tests (no GPU required)
// ---------------------------------------------------------------------------

TEST_CASE("VkmResourceHandle - equality and validity") {
    vkm::VkmResourceHandle a{42, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Texture};
    vkm::VkmResourceHandle b{42, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Texture};
    vkm::VkmResourceHandle c{99, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Buffer};

    CHECK(a == b);
    CHECK(a != c);
    CHECK(a.isValid());
    CHECK(a.isPooledResource());
    CHECK_FALSE(vkm::VKM_INVALID_RESOURCE_HANDLE.isValid());
    CHECK_FALSE(vkm::VKM_INVALID_RESOURCE_HANDLE.isPooledResource());
}

TEST_CASE("VkmResourceHandle - Sampler/TextureView/BufferView are distinct valid pooled types") {
    vkm::VkmResourceHandle sampler{1, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Sampler};
    vkm::VkmResourceHandle textureView{1, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::TextureView};
    vkm::VkmResourceHandle bufferView{1, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::BufferView};

    CHECK(sampler.isValid());
    CHECK(textureView.isValid());
    CHECK(bufferView.isValid());
    CHECK(sampler.isPooledResource());
    CHECK(textureView.isPooledResource());
    CHECK(bufferView.isPooledResource());
    CHECK(sampler != textureView);
    CHECK(textureView != bufferView);
    CHECK(sampler != bufferView);
}

TEST_CASE("VkmResourceHandle - std::hash is consistent and distinguishes distinct handles") {
    std::hash<vkm::VkmResourceHandle> hasher;
    vkm::VkmResourceHandle a{1, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Texture};
    vkm::VkmResourceHandle b{2, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Texture};
    CHECK(hasher(a) == hasher(a));
    CHECK(hasher(a) != hasher(b));
}

TEST_CASE("VkmResourceHandle - the pool type participates in equality and hash") {
    vkm::VkmResourceHandle defaultPool{42, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Texture};
    vkm::VkmResourceHandle transientPool{42, vkm::VkmResourcePoolType::Transient, vkm::VkmResourceType::Texture};
    CHECK(defaultPool != transientPool);
    CHECK(transientPool.isValid());
    CHECK(transientPool.isPooledResource());
    std::hash<vkm::VkmResourceHandle> hasher;
    CHECK(hasher(defaultPool) != hasher(transientPool));
    // Undefined tracks Count, which the bounds checks in VkmRenderResourcePool rely on.
    CHECK(vkm::VKM_INVALID_RESOURCE_HANDLE.poolType == vkm::VkmResourcePoolType::Undefined);
    CHECK_FALSE(vkm::VKM_INVALID_RESOURCE_HANDLE.isPooledResource());
}

TEST_CASE("VkmResourceHandle - generation participates in equality and hash") {
    vkm::VkmResourceHandle a{5, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Texture, 0};
    vkm::VkmResourceHandle b{5, vkm::VkmResourcePoolType::Default, vkm::VkmResourceType::Texture, 1};
    CHECK(a != b);
    std::hash<vkm::VkmResourceHandle> hasher;
    CHECK(hasher(a) != hasher(b));
}

TEST_CASE("VkmFormat - hasDepth returns true only for depth-bearing formats") {
    CHECK(vkm::hasDepth(vkm::VkmFormat::D32_SFLOAT));
    CHECK(vkm::hasDepth(vkm::VkmFormat::D24_UNORM_S8_UINT));
    CHECK(vkm::hasDepth(vkm::VkmFormat::D32_SFLOAT_S8_UINT));
    CHECK_FALSE(vkm::hasDepth(vkm::VkmFormat::R8G8B8A8_UNORM));
    CHECK_FALSE(vkm::hasDepth(vkm::VkmFormat::R8G8B8A8_SRGB));
    CHECK_FALSE(vkm::hasDepth(vkm::VkmFormat::R16G16B16A16_SFLOAT));
    CHECK_FALSE(vkm::hasDepth(vkm::VkmFormat::R32G32B32A32_SFLOAT));
    CHECK_FALSE(vkm::hasDepth(vkm::VkmFormat::Undefined));
}

TEST_CASE("VkmFormat - hasStencil returns true only for stencil-bearing formats") {
    CHECK(vkm::hasStencil(vkm::VkmFormat::D24_UNORM_S8_UINT));
    CHECK(vkm::hasStencil(vkm::VkmFormat::D32_SFLOAT_S8_UINT));
    CHECK_FALSE(vkm::hasStencil(vkm::VkmFormat::D32_SFLOAT));
    CHECK_FALSE(vkm::hasStencil(vkm::VkmFormat::R8G8B8A8_UNORM));
    CHECK_FALSE(vkm::hasStencil(vkm::VkmFormat::Undefined));
}

namespace {
class MockGpuEventTimeline : public vkm::VkmGpuEventTimelineBase {
public:
    MockGpuEventTimeline() : vkm::VkmGpuEventTimelineBase(nullptr) {}
    uint64_t queryLastCompletedTimeline() override { return _lastCompletedCachedTimeline; }
    void waitIdle(const uint64_t) override {}
    void setLastCompleted(uint64_t value) { _lastCompletedCachedTimeline = value; }
};

class MockRenderResource : public vkm::VkmRenderResource {
public:
    MockRenderResource() : vkm::VkmRenderResource(nullptr) {}
    vkm::VkmResourceType getResourceType() const override { return vkm::VkmResourceType::Buffer; }
    void setDebugName(const char*) override {}
};
} // namespace

TEST_CASE("VkmGpuEventTimelineBase - allocateGpuEventTimelineObject increments monotonically") {
    MockGpuEventTimeline tl;
    CHECK(tl.getLastAllocatedTimeline() == 0);

    auto obj1 = tl.allocateGpuEventTimelineObject();
    CHECK(obj1._gpuEventTimeline == &tl);
    CHECK(obj1._timelineValue == 1);
    CHECK(tl.getLastAllocatedTimeline() == 1);

    auto obj2 = tl.allocateGpuEventTimelineObject();
    CHECK(obj2._timelineValue == 2);
    CHECK(obj2._timelineValue > obj1._timelineValue);
    CHECK(tl.getLastAllocatedTimeline() == 2);
}

TEST_CASE("VkmGpuEventTimelineBase - the submitted timeline trails the allocated one") {
    // beginCommandBuffer() allocates a timeline value and only submit() hands one to the GPU, so
    // the two counters are not interchangeable: waiting on the ALLOCATED value waits for something
    // nothing will ever signal as soon as one command buffer is begun and dropped, and the drain
    // then times out silently while resources are destroyed in flight.
    MockGpuEventTimeline tl;
    CHECK(tl.getLastSubmittedTimeline() == 0);

    const auto begun = tl.allocateGpuEventTimelineObject();       // beginCommandBuffer()
    const auto submitted = tl.allocateGpuEventTimelineObject();   // submit()
    tl.markTimelineSubmitted(submitted._timelineValue);
    CHECK(tl.getLastSubmittedTimeline() == submitted._timelineValue);
    CHECK(tl.getLastSubmittedTimeline() > begun._timelineValue);

    // A command buffer begun after the last submit and then never submitted -- the stranded case.
    // The submitted value must not follow it, or draining waits forever.
    const auto stranded = tl.allocateGpuEventTimelineObject();
    CHECK(tl.getLastAllocatedTimeline() == stranded._timelineValue);
    CHECK(tl.getLastSubmittedTimeline() < stranded._timelineValue);

    // Monotonic: Metal signals the highest value among the command buffers in one submit, so an
    // out-of-order mark must not walk the value backwards.
    tl.markTimelineSubmitted(1);
    CHECK(tl.getLastSubmittedTimeline() == submitted._timelineValue);
}

TEST_CASE("VkmRenderResource - recordUsage/getLastUsage/hasAnyPendingUsage tracks per-queue timelines") {
    MockRenderResource resource;
    CHECK_FALSE(resource.hasAnyPendingUsage());

    MockGpuEventTimeline timeline;
    auto usage = timeline.allocateGpuEventTimelineObject(); // timelineValue == 1

    resource.recordUsage(usage);
    CHECK(resource.getLastUsage(usage._gpuEventTimeline)._timelineValue == 1);
    CHECK(resource.hasAnyPendingUsage()); // completed cache still 0 < 1

    timeline.setLastCompleted(1); // simulate the GPU catching up
    CHECK_FALSE(resource.hasAnyPendingUsage());

    // A later usage on the same queue overwrites the earlier one -- only the latest matters.
    auto usage2 = timeline.allocateGpuEventTimelineObject(); // timelineValue == 2
    resource.recordUsage(usage2);
    CHECK(resource.getLastUsage(usage2._gpuEventTimeline)._timelineValue == 2);
    CHECK(resource.hasAnyPendingUsage()); // completed cache is 1 < 2

    timeline.setLastCompleted(2);
    CHECK_FALSE(resource.hasAnyPendingUsage());

    // A second, independent timeline, simulating a second queue instance of the same type, must
    // not clobber the first's tracked entry. Keying usages by queue TYPE alone cannot distinguish
    // two instances of one type.
    MockGpuEventTimeline timeline2;
    auto usage3 = timeline2.allocateGpuEventTimelineObject(); // timelineValue == 1 on timeline2
    resource.recordUsage(usage3);
    CHECK(resource.getLastUsage(usage3._gpuEventTimeline)._timelineValue == 1);
    CHECK(resource.getLastUsage(usage2._gpuEventTimeline)._timelineValue == 2); // first timeline's entry untouched
    CHECK(resource.hasAnyPendingUsage()); // timeline2 not yet completed

    timeline2.setLastCompleted(1);
    CHECK_FALSE(resource.hasAnyPendingUsage()); // both timelines now complete
}

namespace {
// Minimal VkmDriverBase/VkmCommandQueueBase/VkmBuffer stand-ins so
// VkmDeferredResourceReclaimer can be exercised without a real GPU backend. Only the
// initialize()/allocateBuffer() paths actually used by the test below are meaningful --
// the rest are inert stubs required purely to satisfy pure virtuals.
class FakeCommandQueue : public vkm::VkmCommandQueueBase {
public:
    FakeCommandQueue(vkm::VkmDriverBase* driver) : vkm::VkmCommandQueueBase(driver) {}
    void setDebugName(const char*) override {}
protected:
    vkm::VkmGpuEventTimelineObject submitInner(const vkm::CommandSubmitInfo&) override { return {}; }
    bool initializeInner() override { return true; }
};

class MockTexture : public vkm::VkmTexture {
public:
    MockTexture(vkm::VkmDriverBase* driver) : vkm::VkmTexture(driver) {}
    bool initialize(vkm::VkmResourceHandle handle, const vkm::VkmTextureInfo& info) override { return initializeTextureCommon(handle, info); }
    bool overrideExternalHandle(void*) override { return true; }
    void setDebugName(const char*) override {}
};

class MockTextureView : public vkm::VkmTextureView {
public:
    MockTextureView(vkm::VkmDriverBase* driver) : vkm::VkmTextureView(driver) {}
    bool initialize(vkm::VkmResourceHandle handle, const vkm::VkmTextureViewInfo& info) override { return initializeTextureViewCommon(handle, info); }
    void setDebugName(const char*) override {}
};

class FakeDriver : public vkm::VkmDriverBase {
protected:
    vkm::VkmInitResult initializeInner(const vkm::VkmEngineLaunchOptions*) override
    {
        return vkm::VkmInitResult{vkm::VkmInitResultCode::Success, ""};
    }
    void destroyInner() override {}
    vkm::VkmTexture* newTextureInner() override { return new MockTexture(this); }
    vkm::VkmBuffer* newBufferInner() override { return nullptr; }
    vkm::VkmStagingBuffer* newStagingBufferInner() override { return nullptr; }
    vkm::VkmSampler* newSamplerInner() override { return nullptr; }
    vkm::VkmTextureView* newTextureViewInner() override { return new MockTextureView(this); }
    vkm::VkmBufferView* newBufferViewInner() override { return nullptr; }
    vkm::VkmSwapChainBase* newSwapChainInner() override { return nullptr; }
    vkm::VkmResourceTableBase* newResourceTableInner() override { return nullptr; }
    // The mock never reports RayTracing, so VkmDriverBase rejects the call before this runs.
    vkm::VkmAccelerationStructure* newAccelerationStructureInner() override { return nullptr; }
    vkm::VkmCommandQueueBase* newCommandQueueInner() override { return new FakeCommandQueue(this); }
    vkm::VkmPipelineStateBase* newPipelineStateInner() override { return nullptr; }
    vkm::VkmRenderResourcePool* newRenderResourcePoolInner() override { return new vkm::VkmRenderResourcePool(this); }
    vkm::VkmFormat selectSwapChainColorFormat(bool) const override { return vkm::VkmFormat::BGRA8_UNORM; }
};

class MockBuffer : public vkm::VkmBuffer {
public:
    MockBuffer(vkm::VkmDriverBase* driver) : vkm::VkmBuffer(driver) {}
    bool initialize(vkm::VkmResourceHandle, const vkm::VkmBufferInfo&) override { return true; }
    bool overrideExternalHandle(void*) override { return true; }
    void setDebugName(const char*) override {}
};
} // namespace

TEST_CASE("VkmDeferredResourceReclaimer - pollOnce releases only once every recorded usage completes") {
    FakeDriver driver;
    vkm::VkmInitResult initResult = driver.initialize(nullptr);
    REQUIRE(initResult.code == vkm::VkmInitResultCode::Success);

    // A fresh, never-started() reclaimer -- driven purely via pollOnce(), never the real
    // background thread the driver's own _deferredReclaimer already started internally.
    vkm::VkmDeferredResourceReclaimer testReclaimer(&driver);

    vkm::VkmResourceHandle handle = driver.getRenderResourcePool()->allocateBuffer(new MockBuffer(&driver));
    REQUIRE(handle.isValid());

    MockGpuEventTimeline timeline;
    auto usage = timeline.allocateGpuEventTimelineObject(); // timelineValue == 1
    driver.getRenderResourcePool()->getResource<vkm::VkmBuffer>(handle)->recordUsage(usage);

    testReclaimer.requestRelease(handle);

    testReclaimer.pollOnce();
    CHECK(driver.getRenderResourcePool()->getResource<vkm::VkmBuffer>(handle) != nullptr); // still pending

    timeline.setLastCompleted(1);
    testReclaimer.pollOnce();
    CHECK(driver.getRenderResourcePool()->getResource<vkm::VkmBuffer>(handle) == nullptr); // now released

    driver.destroy();
}

TEST_CASE("VkmTexture::createView - owns its views and cascading release waits for both") {
    FakeDriver driver;
    REQUIRE(driver.initialize(nullptr).code == vkm::VkmInitResultCode::Success);

    vkm::VkmTextureInfo textureInfo{};
    vkm::VkmTexture* texture = driver.newTexture(textureInfo);
    REQUIRE(texture != nullptr);

    vkm::VkmTextureViewInfo viewInfo{};
    vkm::VkmTextureView* view = texture->createView(viewInfo);
    REQUIRE(view != nullptr);
    CHECK(view->getTextureViewInfo()._texture == texture->getHandle());
    CHECK(texture->getOwnedChildHandles() == std::vector<vkm::VkmResourceHandle>{view->getHandle()});
    CHECK(view->isParentAlive());
    CHECK(view->tryGetParent() == texture);

    // A fresh, never-started() reclaimer -- driven purely via pollOnce(), mirroring the
    // existing MockBuffer reclaimer test's pattern.
    vkm::VkmDeferredResourceReclaimer testReclaimer(&driver);

    MockGpuEventTimeline timeline;
    auto usage = timeline.allocateGpuEventTimelineObject(); // timelineValue == 1
    texture->recordUsage(usage);

    testReclaimer.requestRelease(texture->getHandle());

    // The view has no recorded usage of its own, so its own pending entry is immediately
    // ready and gets released on the first pollOnce() -- but the parent's entry checks (in
    // that SAME locked pass) whether the child is *already* released, which it isn't yet at
    // that point (release happens after the lock, in the same pollOnce() call), so the
    // parent stays pending for at least one more pollOnce().
    testReclaimer.pollOnce();
    CHECK(driver.getRenderResourcePool()->getResource<vkm::VkmTextureView>(view->getHandle()) == nullptr);
    CHECK(driver.getRenderResourcePool()->getResource<vkm::VkmTexture>(texture->getHandle()) != nullptr);

    timeline.setLastCompleted(1);
    testReclaimer.pollOnce();
    CHECK(driver.getRenderResourcePool()->getResource<vkm::VkmTexture>(texture->getHandle()) == nullptr);

    driver.destroy();
}

TEST_CASE("VkmRenderResourcePool - tagResource tracks per-category memory usage and decrements on release") {
    FakeDriver driver;
    REQUIRE(driver.initialize(nullptr).code == vkm::VkmInitResultCode::Success);
    vkm::VkmRenderResourcePool* pool = driver.getRenderResourcePool();

    vkm::VkmResourceHandle h1 = pool->allocateBuffer(new MockBuffer(&driver));
    vkm::VkmResourceHandle h2 = pool->allocateBuffer(new MockBuffer(&driver));
    REQUIRE(h1.isValid());
    REQUIRE(h2.isValid());

    vkm::VkmResourceMemoryTag tag1{};
    tag1.requestedSize = 100;
    tag1.allocatedSize = 128;
    tag1.alignment = 16;
    tag1.name = "buffer1";
    tag1.type = vkm::VkmResourceType::Buffer;
    pool->tagResource(h1, tag1);

    vkm::VkmResourceMemoryTag tag2{};
    tag2.requestedSize = 200;
    tag2.allocatedSize = 256;
    tag2.type = vkm::VkmResourceType::Buffer;
    pool->tagResource(h2, tag2);

    auto usage = pool->getCategoryMemoryUsage(vkm::VkmResourceType::Buffer);
    CHECK(usage.totalRequestedBytes == 300);
    CHECK(usage.totalAllocatedBytes == 384);
    CHECK(usage.liveCount == 2);

    auto total = pool->getTotalMemoryUsage();
    CHECK(total.totalRequestedBytes == 300);
    CHECK(total.totalAllocatedBytes == 384);

    auto queried = pool->getResourceMemoryTag(h1);
    REQUIRE(queried.has_value());
    CHECK(queried->name == "buffer1");

    pool->releaseResource(h1);
    CHECK_FALSE(pool->getResourceMemoryTag(h1).has_value());

    auto usageAfterRelease = pool->getCategoryMemoryUsage(vkm::VkmResourceType::Buffer);
    CHECK(usageAfterRelease.totalRequestedBytes == 200);
    CHECK(usageAfterRelease.totalAllocatedBytes == 256);
    CHECK(usageAfterRelease.liveCount == 1);

    pool->releaseResource(h2);
    driver.destroy();
}

TEST_CASE("VkmRenderResourcePool - the Transient sub-pool is tracked independently of Default") {
    FakeDriver driver;
    REQUIRE(driver.initialize(nullptr).code == vkm::VkmInitResultCode::Success);
    vkm::VkmRenderResourcePool* pool = driver.getRenderResourcePool();

    vkm::VkmResourceHandle defaultHandle = pool->allocateTexture(new MockTexture(&driver));
    vkm::VkmResourceHandle transientHandle =
        pool->allocateTexture(new MockTexture(&driver), vkm::VkmResourcePoolType::Transient);
    REQUIRE(defaultHandle.isValid());
    REQUIRE(transientHandle.isValid());

    CHECK(transientHandle.poolType == vkm::VkmResourcePoolType::Transient);
    CHECK(transientHandle.isPooledResource());
    // Each sub-pool has its own id space, so the two collide on id and are still distinct
    // handles -- which is the property the pool-type field exists to provide.
    CHECK(defaultHandle.id == transientHandle.id);
    CHECK(defaultHandle != transientHandle);
    CHECK(std::hash<vkm::VkmResourceHandle>()(defaultHandle) != std::hash<vkm::VkmResourceHandle>()(transientHandle));

    vkm::VkmResourceMemoryTag defaultTag{};
    defaultTag.requestedSize = 100;
    defaultTag.allocatedSize = 128;
    defaultTag.type = vkm::VkmResourceType::Texture;
    pool->tagResource(defaultHandle, defaultTag);

    vkm::VkmResourceMemoryTag transientTag{};
    transientTag.requestedSize = 200;
    transientTag.allocatedSize = 0;
    transientTag.metadata = "transient";
    transientTag.type = vkm::VkmResourceType::Texture;
    pool->tagResource(transientHandle, transientTag);

    auto queried = pool->getResourceMemoryTag(transientHandle);
    REQUIRE(queried.has_value());
    CHECK(queried->metadata == "transient");
    CHECK(queried->allocatedSize == 0);

    // Category totals and handle enumeration sum across every sub-pool, so both show up here.
    auto usage = pool->getCategoryMemoryUsage(vkm::VkmResourceType::Texture);
    CHECK(usage.totalRequestedBytes == 300);
    CHECK(usage.totalAllocatedBytes == 128);
    CHECK(usage.liveCount == 2);

    std::vector<vkm::VkmResourceHandle> handles = pool->getAllResourceHandles(vkm::VkmResourceType::Texture);
    CHECK(std::find(handles.begin(), handles.end(), defaultHandle) != handles.end());
    CHECK(std::find(handles.begin(), handles.end(), transientHandle) != handles.end());

    pool->releaseResource(transientHandle);
    CHECK(pool->getResource<vkm::VkmTexture>(transientHandle) == nullptr);
    CHECK(pool->getResource<vkm::VkmTexture>(defaultHandle) != nullptr);
    CHECK(pool->getCategoryMemoryUsage(vkm::VkmResourceType::Texture).liveCount == 1);

    pool->releaseResource(defaultHandle);
    driver.destroy();
}

TEST_CASE("captureMemorySnapshot aggregates the CPU tracker and the GPU pool into one sample") {
    FakeDriver driver;
    REQUIRE(driver.initialize(nullptr).code == vkm::VkmInitResultCode::Success);
    vkm::VkmRenderResourcePool* pool = driver.getRenderResourcePool();

    vkm::VkmResourceHandle bufferHandle = pool->allocateBuffer(new MockBuffer(&driver));
    vkm::VkmResourceHandle textureHandle = pool->allocateTexture(new MockTexture(&driver));
    REQUIRE(bufferHandle.isValid());
    REQUIRE(textureHandle.isValid());

    vkm::VkmResourceMemoryTag bufferTag{};
    bufferTag.requestedSize = 1000;
    bufferTag.allocatedSize = 1024;
    bufferTag.type = vkm::VkmResourceType::Buffer;
    pool->tagResource(bufferHandle, bufferTag);

    vkm::VkmResourceMemoryTag textureTag{};
    textureTag.requestedSize = 4000;
    textureTag.allocatedSize = 4096;
    textureTag.type = vkm::VkmResourceType::Texture;
    pool->tagResource(textureHandle, textureTag);

    const vkm::VkmMemorySnapshot snapshot = vkm::captureMemorySnapshot(&driver);

    // GPU side mirrors the pool's own totals, per category and summed.
    const size_t bufferIndex = static_cast<size_t>(vkm::VkmResourceType::Buffer);
    const size_t textureIndex = static_cast<size_t>(vkm::VkmResourceType::Texture);
    CHECK(snapshot._gpuByCategory[bufferIndex].totalAllocatedBytes == 1024);
    CHECK(snapshot._gpuByCategory[textureIndex].totalAllocatedBytes == 4096);
    CHECK(snapshot._gpuTotal.totalAllocatedBytes == 1024 + 4096);
    CHECK(snapshot._gpuTotal.totalRequestedBytes == 1000 + 4000);
    CHECK(snapshot._gpuTotal.liveCount == 2);
    // FakeDriver has no backing API, so it keeps VkmDriverBase's "nothing to report" default.
    CHECK_FALSE(snapshot._gpu._hasDeviceStats);

    // CPU side: the totals must be exactly the sum over the tags that were kept, and every
    // kept tag must be live (fully-freed tags are dropped, not reported as zero rows).
    uint64_t summedUsable = 0;
    uint64_t summedRequested = 0;
    uint64_t summedLive = 0;
    for (const vkm::TaggedAllocationSummary& tag : snapshot._cpuTags)
    {
        CHECK(tag.liveCount > 0);
        summedUsable += tag.usableBytes;
        summedRequested += tag.requestedBytes;
        summedLive += tag.liveCount;
    }
    CHECK(snapshot._cpuTrackedUsableBytes == summedUsable);
    CHECK(snapshot._cpuTrackedRequestedBytes == summedRequested);
    CHECK(snapshot._cpuTrackedLiveCount == summedLive);
    CHECK(summedLive > 0); // this test's own allocations are in there

    CHECK(std::is_sorted(snapshot._cpuTags.begin(), snapshot._cpuTags.end(),
                         [](const vkm::TaggedAllocationSummary& lhs, const vkm::TaggedAllocationSummary& rhs) {
                             return lhs.usableBytes > rhs.usableBytes;
                         }));

    // Drives every formatting branch of the shutdown dump (VkmEngine::destroy() calls this
    // with a real driver), including the per-tag and per-category loops.
    CHECK_NOTHROW(vkm::logMemoryReport(snapshot));

    pool->releaseResource(bufferHandle);
    pool->releaseResource(textureHandle);
    driver.destroy();
}

TEST_CASE("captureMemorySnapshot tolerates a null driver") {
    const vkm::VkmMemorySnapshot snapshot = vkm::captureMemorySnapshot(nullptr);

    CHECK(snapshot._gpuTotal.liveCount == 0);
    CHECK_FALSE(snapshot._gpu._hasDeviceStats);
    CHECK(snapshot._cpuTrackedLiveCount > 0); // the CPU half is still filled in
}

TEST_CASE("formatByteSize picks a readable unit") {
    CHECK(vkm::formatByteSize(512) == "512 B");
    CHECK(vkm::formatByteSize(2048) == "2.0 KiB");
    CHECK(vkm::formatByteSize(3ull * 1024 * 1024) == "3.0 MiB");
    CHECK(vkm::formatByteSize(2ull * 1024 * 1024 * 1024) == "2.00 GiB");
}

TEST_CASE("VkmRenderResourcePool - releaseResource recycles the id with a new generation, rejecting the stale handle") {
    FakeDriver driver;
    REQUIRE(driver.initialize(nullptr).code == vkm::VkmInitResultCode::Success);
    vkm::VkmRenderResourcePool* pool = driver.getRenderResourcePool();

    vkm::VkmResourceHandle staleHandle = pool->allocateBuffer(new MockBuffer(&driver));
    REQUIRE(staleHandle.isValid());
    REQUIRE(pool->getResource<vkm::VkmBuffer>(staleHandle) != nullptr);

    pool->releaseResource(staleHandle);
    CHECK(pool->getResource<vkm::VkmBuffer>(staleHandle) == nullptr);

    vkm::VkmResourceHandle recycledHandle = pool->allocateBuffer(new MockBuffer(&driver));
    CHECK(recycledHandle.id == staleHandle.id); // free-list reused the same id
    CHECK(recycledHandle.generation != staleHandle.generation); // but with a bumped generation

    // The stale (old-generation) handle must still be rejected even though it shares the
    // same id with the newly-recycled, live handle.
    CHECK(pool->getResource<vkm::VkmBuffer>(staleHandle) == nullptr);
    CHECK(pool->getResource<vkm::VkmBuffer>(recycledHandle) != nullptr);

    pool->releaseResource(recycledHandle);
    driver.destroy();
}

TEST_CASE("VkmEngineLaunchOptions - parseEngineLaunchOptions parses --enable-gpu-capture") {
    const char* rawArgs[] = { "vkm", "--enable-gpu-capture=true" };
    vkm::VkmEngineLaunchOptions options = vkm::VkmEngine::parseEngineLaunchOptions(2, const_cast<char**>(rawArgs));
    CHECK(options.enableGpuCapture);
}

TEST_CASE("VkmDriverBase - isDebugNamingEnabled reflects launch options, defaults safely on null options") {
    SUBCASE("null options -> naming disabled regardless of DEFAULT_ENGINE_LAUNCH_OPTIONS") {
        FakeDriver driver;
        REQUIRE(driver.initialize(nullptr).code == vkm::VkmInitResultCode::Success);
        CHECK_FALSE(driver.isDebugNamingEnabled());
        driver.destroy();
    }
    SUBCASE("both flags false -> naming disabled") {
        FakeDriver driver;
        vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = false, .enableGpuCapture = false };
        REQUIRE(driver.initialize(&opts).code == vkm::VkmInitResultCode::Success);
        CHECK_FALSE(driver.isDebugNamingEnabled());
        driver.destroy();
    }
    SUBCASE("enableValidationLayer alone -> naming enabled") {
        FakeDriver driver;
        vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true, .enableGpuCapture = false };
        REQUIRE(driver.initialize(&opts).code == vkm::VkmInitResultCode::Success);
        CHECK(driver.isDebugNamingEnabled());
        driver.destroy();
    }
    SUBCASE("enableGpuCapture alone -> naming enabled") {
        FakeDriver driver;
        vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = false, .enableGpuCapture = true };
        REQUIRE(driver.initialize(&opts).code == vkm::VkmInitResultCode::Success);
        CHECK(driver.isDebugNamingEnabled());
        driver.destroy();
    }
}

TEST_CASE("VkmOffsetAllocator - allocate returns valid, non-overlapping ranges") {
    vkm::VkmOffsetAllocator allocator(1024);

    auto a = allocator.allocate(64, 1);
    auto b = allocator.allocate(128, 1);
    CHECK(a.isValid());
    CHECK(b.isValid());
    CHECK(a._offset != b._offset);

    allocator.free(a);
    allocator.free(b);
}

TEST_CASE("VkmOffsetAllocator - alignment padding returns correctly aligned offsets") {
    vkm::VkmOffsetAllocator allocator(4096);

    // Force an unaligned starting point, then request a 256-aligned allocation.
    auto filler = allocator.allocate(3, 1);
    CHECK(filler.isValid());

    auto aligned = allocator.allocate(64, 256);
    CHECK(aligned.isValid());
    CHECK((aligned._offset % 256) == 0);

    allocator.free(aligned);
    allocator.free(filler);
}

TEST_CASE("VkmOffsetAllocator - free then reallocate reuses freed space") {
    vkm::VkmOffsetAllocator allocator(256);

    auto a = allocator.allocate(256, 1);
    CHECK(a.isValid());

    // Pool is full; a second allocation of any size must fail.
    auto b = allocator.allocate(1, 1);
    CHECK_FALSE(b.isValid());

    allocator.free(a);

    // Freed space must be reusable.
    auto c = allocator.allocate(256, 1);
    CHECK(c.isValid());
    allocator.free(c);
}

TEST_CASE("VkmOffsetAllocator - allocation exceeding pool size fails") {
    vkm::VkmOffsetAllocator allocator(128);
    auto a = allocator.allocate(256, 1);
    CHECK_FALSE(a.isValid());
}

// ---------------------------------------------------------------------------
// Vulkan headless driver + swapchain tests
// ---------------------------------------------------------------------------

#ifdef VKM_USE_VULKAN_API

struct VulkanDriverFixture {
    std::unique_ptr<vkm::VkmDriverVulkan> driver;
    vkm::VkmInitResult initResult;
    VulkanDriverFixture() {
        glfwInit();
        vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
        driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
        initResult = driver->initialize(&opts);
    }
    ~VulkanDriverFixture() {
        // destroy() must run before the driver object is deleted: it tears down driver-owned pools
        // -- VmaAllocator, buffer pools, the deferred reclaimer thread -- that pooled resources'
        // destructors depend on.
        if (driver)
        {
            driver->destroy();
        }
        driver.reset();
        glfwTerminate();
    }
};

TEST_CASE("VkmDriverVulkan - initialization succeeds") {
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    SUBCASE("render resource pool is available") {
        CHECK(f.driver->getRenderResourcePool() != nullptr);
    }
    SUBCASE("graphics command queue is created") {
        CHECK(f.driver->getCommandQueue(vkm::VkmCommandQueueType::Graphics, 0) != nullptr);
    }
    SUBCASE("compute command queue is created") {
        CHECK(f.driver->getCommandQueue(vkm::VkmCommandQueueType::Compute, 0) != nullptr);
    }
    SUBCASE("Vulkan handles are valid after init") {
        CHECK(f.driver->getDevice() != VK_NULL_HANDLE);
        CHECK(f.driver->getPhysicalDevice() != VK_NULL_HANDLE);
        CHECK(f.driver->getInstance() != VK_NULL_HANDLE);
    }
    SUBCASE("CommandBufferReusable capability flag is set on Vulkan") {
        CHECK((f.driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::CommandBufferReusable) != 0);
    }
    SUBCASE("RayTracing implies BufferDeviceAddress") {
        // Not a redundant pair. An acceleration structure is built from geometry addressed by
        // device address (Vulkan's VK_KHR_acceleration_structure requires the
        // bufferDeviceAddress feature outright), so a backend that claims to trace rays but
        // cannot report a buffer's GPU address has nothing to build one from. Asserted as an
        // implication rather than a value because the answer is per-device: MoltenVK exposes no
        // RT extensions at all, and lavapipe only does from Mesa 24.1.
        const uint32_t flags = static_cast<uint32_t>(f.driver->getDriverCapabilityFlags());
        const bool rayTracing = (flags & static_cast<uint32_t>(vkm::VkmDriverCapabilityFlags::RayTracing)) != 0u;
        const bool deviceAddress =
            (flags & static_cast<uint32_t>(vkm::VkmDriverCapabilityFlags::BufferDeviceAddress)) != 0u;
        // Logged, not asserted: whether a device offers ray tracing is a property of the machine
        // rather than of the code.
        if (rayTracing) { MESSAGE("RayTracing capability on this device: yes"); }
        else            { MESSAGE("RayTracing capability on this device: no"); }
        CHECK((!rayTracing || deviceAddress));

        /*
        * Texture streaming's high tier changes which mip levels are backed rather than rebuilding
        * the texture, so it is meaningless without a bindless array to leave the slot pointing at.
        * An implication again, not a value: placement sparse is a hardware tier on Metal and a
        * feature bit on Vulkan, and MoltenVK reports neither.
        */
        const bool sparse = (flags & static_cast<uint32_t>(vkm::VkmDriverCapabilityFlags::SparseResidency)) != 0u;
        const bool bindless = (flags & static_cast<uint32_t>(vkm::VkmDriverCapabilityFlags::BindlessTextures)) != 0u;
        if (sparse) { MESSAGE("SparseResidency capability on this device: yes"); }
        else        { MESSAGE("SparseResidency capability on this device: no"); }
        CHECK((!sparse || bindless));
    }
}

TEST_CASE("VkmDriverVulkan - VmaAllocator is created on init") {
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    CHECK(f.driver->getVmaAllocator() != nullptr);
}

TEST_CASE("VkmDriverVulkan - newBuffer/newStagingBuffer/newSampler create valid resources") {
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    SUBCASE("committed buffer") {
        vkm::VkmBufferInfo info{};
        info._flags = vkm::VkmResourceCreateInfo::AllowShaderRead;
        info._size = 256;
        info._placementHint = vkm::VkmMemoryPlacementHint::Committed;
        vkm::VkmBuffer* buffer = f.driver->newBuffer(info);
        REQUIRE(buffer != nullptr);
        CHECK(buffer->getHandle().isValid());
        auto* vkBuffer = f.driver->getRenderResourcePool()->getResource<vkm::VkmBufferVulkan>(buffer->getHandle());
        REQUIRE(vkBuffer != nullptr);
        CHECK(vkBuffer->getBuffer() != VK_NULL_HANDLE);
        // Never GPU-used (no timeline recorded), so an immediate release is correct here --
        // requestRelease() is only needed once a resource may have been submitted.
        f.driver->getRenderResourcePool()->releaseResource(buffer->getHandle());
    }

    SUBCASE("heap-placed buffer") {
        vkm::VkmBufferInfo info{};
        info._flags = vkm::VkmResourceCreateInfo::AllowShaderRead;
        info._size = 256;
        info._placementHint = vkm::VkmMemoryPlacementHint::Heap;
        vkm::VkmBuffer* buffer = f.driver->newBuffer(info);
        REQUIRE(buffer != nullptr);
        auto* vkBuffer = f.driver->getRenderResourcePool()->getResource<vkm::VkmBufferVulkan>(buffer->getHandle());
        REQUIRE(vkBuffer != nullptr);
        CHECK(vkBuffer->getBuffer() != VK_NULL_HANDLE);
        f.driver->getRenderResourcePool()->releaseResource(buffer->getHandle());
    }

    SUBCASE("staging buffer is host-mapped") {
        vkm::VkmStagingBufferInfo info{};
        info._flags = vkm::VkmResourceCreateInfo::AllowTransferSrc;
        info._size = 256;
        vkm::VkmStagingBuffer* stagingBuffer = f.driver->newStagingBuffer(info);
        REQUIRE(stagingBuffer != nullptr);
        CHECK(stagingBuffer->map() != nullptr);
        f.driver->getRenderResourcePool()->releaseResource(stagingBuffer->getHandle());
    }

    SUBCASE("sampler") {
        vkm::VkmSamplerInfo info{};
        vkm::VkmSampler* sampler = f.driver->newSampler(info);
        REQUIRE(sampler != nullptr);
        auto* vkSampler = f.driver->getRenderResourcePool()->getResource<vkm::VkmSamplerVulkan>(sampler->getHandle());
        REQUIRE(vkSampler != nullptr);
        CHECK(vkSampler->getSampler() != VK_NULL_HANDLE);
        f.driver->getRenderResourcePool()->releaseResource(sampler->getHandle());
    }
}

TEST_CASE("VkmDriverVulkan - committed buffer allocation is tagged with real VMA size/alignment") {
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    vkm::VkmBufferInfo info{};
    info._flags = vkm::VkmResourceCreateInfo::AllowShaderRead;
    info._size = 250; // deliberately unaligned, so VMA padding is observable
    info._placementHint = vkm::VkmMemoryPlacementHint::Committed;
    info._debugName = "TaggedTestBuffer";
    vkm::VkmBuffer* buffer = f.driver->newBuffer(info);
    REQUIRE(buffer != nullptr);

    auto tag = f.driver->getRenderResourcePool()->getResourceMemoryTag(buffer->getHandle());
    REQUIRE(tag.has_value());
    CHECK(tag->requestedSize == 250);
    CHECK(tag->allocatedSize >= tag->requestedSize);
    CHECK(tag->alignment > 0);
    CHECK((tag->alignment & (tag->alignment - 1)) == 0); // power of two
    CHECK(tag->name == "TaggedTestBuffer");

    f.driver->getRenderResourcePool()->releaseResource(buffer->getHandle());
}

/*
* The assertion that actually pins what Heap means on the Vulkan buffer path. Everything else
* about placement is invisible from the engine API -- two buffers being suballocated from one
* shared VkBuffer is not. If Heap ever silently degraded to a committed allocation, each buffer
* would get its own VkBuffer at offset 0 and the first two CHECKs would fire.
*/
// No comma in the name on purpose: doctest's --test-case= filter splits on commas, so a comma
// here would make this the one test the re-run workflow in tests/CLAUDE.md cannot select.
TEST_CASE("VkmDriverVulkan - Heap buffers share one VkBuffer but Committed buffers do not") {
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    vkm::VkmRenderResourcePool* resourcePool = f.driver->getRenderResourcePool();

    auto makeBuffer = [&](vkm::VkmMemoryPlacementHint placementHint) {
        vkm::VkmBufferInfo info{};
        info._flags = vkm::VkmResourceCreateInfo::AllowShaderRead;
        info._size = 256;
        info._placementHint = placementHint;
        vkm::VkmBuffer* buffer = f.driver->newBuffer(info);
        REQUIRE(buffer != nullptr);
        return buffer;
    };

    vkm::VkmBuffer* heapFirst = makeBuffer(vkm::VkmMemoryPlacementHint::Heap);
    vkm::VkmBuffer* heapSecond = makeBuffer(vkm::VkmMemoryPlacementHint::Heap);
    vkm::VkmBuffer* committed = makeBuffer(vkm::VkmMemoryPlacementHint::Committed);

    auto* vkHeapFirst = resourcePool->getResource<vkm::VkmBufferVulkan>(heapFirst->getHandle());
    auto* vkHeapSecond = resourcePool->getResource<vkm::VkmBufferVulkan>(heapSecond->getHandle());
    auto* vkCommitted = resourcePool->getResource<vkm::VkmBufferVulkan>(committed->getHandle());
    REQUIRE(vkHeapFirst != nullptr);
    REQUIRE(vkHeapSecond != nullptr);
    REQUIRE(vkCommitted != nullptr);

    // Compared as integers, not as VkBuffer: doctest cannot stringify a Vulkan handle and
    // renders every one of them as "1", so a failure on the raw handles would report the
    // useless "CHECK( 1 != 1 )" instead of naming the two buffers that collided.
    const uint64_t heapFirstBuffer = (uint64_t)vkHeapFirst->getBuffer();
    const uint64_t heapSecondBuffer = (uint64_t)vkHeapSecond->getBuffer();
    const uint64_t committedBuffer = (uint64_t)vkCommitted->getBuffer();

    CHECK(heapFirstBuffer == heapSecondBuffer);
    CHECK(vkHeapFirst->getBufferOffset() != vkHeapSecond->getBufferOffset());

    // A committed buffer owns its VkBuffer outright, so it is a different object and its
    // contents start at 0 -- which is what every getBufferOffset() consumer relies on.
    CHECK(committedBuffer != heapFirstBuffer);
    CHECK(vkCommitted->getBufferOffset() == 0);

    resourcePool->releaseResource(committed->getHandle());
    resourcePool->releaseResource(heapSecond->getHandle());
    resourcePool->releaseResource(heapFirst->getHandle());
}

TEST_CASE("VkmDriverVulkan - resource creation succeeds with enableGpuCapture enabled") {
    glfwInit();

    vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true, .enableGpuCapture = true };
    std::unique_ptr<vkm::VkmDriverVulkan> driver(new vkm::VkmDriverVulkan());
    vkm::VkmInitResult initResult = driver->initialize(&opts);
    if (initResult.code == vkm::VkmInitResultCode::HardwareUnsupported) {
        MESSAGE("Skipping: " << initResult.reason);
        glfwTerminate();
        return;
    }
    REQUIRE_MESSAGE(initResult.code == vkm::VkmInitResultCode::Success, initResult.reason);
    CHECK(driver->isDebugNamingEnabled());

    // Per tests/CLAUDE.md, only non-null/functional assertions are permitted here -- actually
    // verifying the native debug label landed requires external tooling (RenderDoc/Xcode),
    // not in-process assertion. This proves naming-enabled resource creation doesn't crash
    // or fail, which is the meaningful in-process guarantee.
    vkm::VkmBufferInfo info{};
    info._flags = vkm::VkmResourceCreateInfo::AllowShaderRead;
    info._size = 256;
    info._debugName = "GpuCaptureTestBuffer";
    vkm::VkmBuffer* buffer = driver->newBuffer(info);
    REQUIRE(buffer != nullptr);
    CHECK(buffer->getHandle().isValid());
    driver->getRenderResourcePool()->releaseResource(buffer->getHandle());

    driver->destroy();
    glfwTerminate();
}

TEST_CASE("VkmDriverVulkan - texture view and buffer view resolve their parent resource") {
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    vkm::VkmTextureInfo textureInfo{};
    textureInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderRead;
    textureInfo._extent = {4, 4, 1};
    textureInfo._numMipLevels = 1;
    textureInfo._numArrayLayers = 1;
    textureInfo._format = vkm::VkmFormat::R8G8B8A8_UNORM;
    vkm::VkmTexture* texture = f.driver->newTexture(textureInfo);
    REQUIRE(texture != nullptr);

    vkm::VkmTextureViewInfo viewInfo{};
    viewInfo._texture = texture->getHandle();
    viewInfo._numMipLevels = 1;
    viewInfo._numArrayLayers = 1;
    vkm::VkmTextureView* textureView = texture->createView(viewInfo);
    REQUIRE(textureView != nullptr);
    auto* vkTextureView = f.driver->getRenderResourcePool()->getResource<vkm::VkmTextureViewVulkan>(textureView->getHandle());
    REQUIRE(vkTextureView != nullptr);
    CHECK(vkTextureView->getImageView() != VK_NULL_HANDLE);

    vkm::VkmBufferInfo bufferInfo{};
    bufferInfo._flags = vkm::VkmResourceCreateInfo::AllowShaderRead;
    bufferInfo._size = 256;
    vkm::VkmBuffer* buffer = f.driver->newBuffer(bufferInfo);
    REQUIRE(buffer != nullptr);

    vkm::VkmBufferViewInfo bufferViewInfo{};
    bufferViewInfo._buffer = buffer->getHandle();
    bufferViewInfo._offset = 0;
    bufferViewInfo._size = 256;
    vkm::VkmBufferView* bufferView = buffer->createView(bufferViewInfo);
    REQUIRE(bufferView != nullptr);
    auto* vkBufferView = f.driver->getRenderResourcePool()->getResource<vkm::VkmBufferViewVulkan>(bufferView->getHandle());
    REQUIRE(vkBufferView != nullptr);
    CHECK(vkBufferView->getOffset() == 0);
    CHECK(vkBufferView->getSize() == 256);

    // Release views before their parent resources -- a view must not outlive what it views.
    vkm::VkmRenderResourcePool* pool = f.driver->getRenderResourcePool();
    pool->releaseResource(bufferView->getHandle());
    pool->releaseResource(buffer->getHandle());
    pool->releaseResource(textureView->getHandle());
    pool->releaseResource(texture->getHandle());
}

TEST_CASE("VkmDriverVulkan - graphics queue exposes a valid VkQueue handle") {
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    auto* queue = static_cast<vkm::VkmCommandQueueVulkan*>(
        f.driver->getCommandQueue(vkm::VkmCommandQueueType::Graphics, 0));
    REQUIRE(queue != nullptr);
    CHECK(queue->getVkQueue() != VK_NULL_HANDLE);
}

TEST_CASE("VkmSwapChainVulkan - created and initialized with a hidden GLFW window") {
    glfwInit();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(256, 256, "UnitTest", nullptr, nullptr);
    if (window == nullptr) {
        MESSAGE("Skipping: glfwCreateWindow failed (no display server available in this environment).");
        glfwTerminate();
        return;
    }

    vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };

    std::unique_ptr<vkm::VkmDriverVulkan> driver(new vkm::VkmDriverVulkan());
    vkm::VkmInitResult initResult = driver->initialize(&opts);
    if (initResult.code == vkm::VkmInitResultCode::HardwareUnsupported) {
        MESSAGE("Skipping: " << initResult.reason);
        glfwDestroyWindow(window);
        glfwTerminate();
        return;
    }
    REQUIRE_MESSAGE(initResult.code == vkm::VkmInitResultCode::Success, initResult.reason);

    // The surface, not the requested window size, decides the real extent: on a HiDPI display
    // the framebuffer is the window size in points times the backing scale.
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    {
        // unique_ptr destroyed before driver — correct order: swapchain uses driver's resource pool
        std::unique_ptr<vkm::VkmSwapChainBase> sc(driver->newSwapChain());
        REQUIRE(sc != nullptr);
        REQUIRE(sc->initialize(vkm::VkmWindowInfo{ 256, 256, "UnitTest", window }));
        CHECK(sc->getExtent() == glm::uvec2((uint32_t)framebufferWidth, (uint32_t)framebufferHeight));
        REQUIRE(sc->getBackBufferCount() > 0);

        SUBCASE("resize to a new extent recreates the back buffers") {
            glfwSetWindowSize(window, 384, 320);
            glfwPollEvents();

            int resizedWidth = 0;
            int resizedHeight = 0;
            glfwGetFramebufferSize(window, &resizedWidth, &resizedHeight);
            // A headless/offscreen window manager may not honour glfwSetWindowSize; there is
            // nothing to assert about a resize that did not happen.
            if (resizedWidth != framebufferWidth || resizedHeight != framebufferHeight) {
                sc->resize((uint32_t)resizedWidth, (uint32_t)resizedHeight);

                CHECK(sc->getExtent() == glm::uvec2((uint32_t)resizedWidth, (uint32_t)resizedHeight));
                REQUIRE(sc->getBackBufferCount() > 0);
                // Every image in the new set must be a live pool resource: resize() releases the
                // old handles and the backend allocates a fresh one per swapchain image.
                for (uint8_t i = 0; i < sc->getBackBufferCount(); ++i) {
                    CAPTURE(i);
                    const vkm::VkmResourceHandle handle = sc->getBackBuffer(i);
                    REQUIRE(handle.isValid());
                    CHECK(driver->getRenderResourcePool()->getResource<vkm::VkmTexture>(handle) != nullptr);
                }
            } else {
                MESSAGE("Skipping: the window manager did not honour glfwSetWindowSize.");
            }
        }

        SUBCASE("resize to the same extent keeps the existing back buffers") {
            std::vector<vkm::VkmResourceHandle> before;
            for (uint8_t i = 0; i < sc->getBackBufferCount(); ++i) {
                before.push_back(sc->getBackBuffer(i));
            }

            sc->resize((uint32_t)framebufferWidth, (uint32_t)framebufferHeight);

            CHECK(sc->getExtent() == glm::uvec2((uint32_t)framebufferWidth, (uint32_t)framebufferHeight));
            REQUIRE((size_t)sc->getBackBufferCount() == before.size());
            for (size_t i = 0; i < before.size(); ++i) {
                CAPTURE(i);
                CHECK(sc->getBackBuffer((uint8_t)i) == before[i]);
            }
        }

        SUBCASE("resize to a zero extent tears the swapchain down") {
            // What a minimized window reports. Nothing is created, and the engine skips the
            // window until a non-zero size brings it back.
            sc->resize(0u, 0u);

            CHECK(sc->getExtent() == glm::uvec2(0u, 0u));
            CHECK(sc->getBackBufferCount() == 0);

            sc->resize((uint32_t)framebufferWidth, (uint32_t)framebufferHeight);
            CHECK(sc->getExtent() == glm::uvec2((uint32_t)framebufferWidth, (uint32_t)framebufferHeight));
            CHECK(sc->getBackBufferCount() > 0);
        }
    }

    driver->destroy();
    driver.reset();
    glfwDestroyWindow(window);
    glfwTerminate();
}

namespace {
// Per tests/CLAUDE.md: no direct graphics API calls -- this only drives the engine's own
// AppDelegate/loopInner() surface, exactly like a real sample would.
struct NullAppDelegate : vkm::AppDelegate {
    void postDriverReady(vkm::VkmEngine*) override {}
    void preShutdown() override {}
    void update(const double) override {}
    void render(uint32_t, vkm::VkmRenderGraph*, vkm::VkmResourceHandle) override {}
    const char* getAppName() const override { return "ImGuiSmokeTest"; }
};
} // namespace

// One TEST_CASE, not two, and deliberately so: VkmEngine::initializeEngine() registers
// process-wide loggers, so a second VkmEngine in the same process throws "logger with name
// 'ConsoleLogger' already exists". This is therefore the single place a live engine can be
// driven, and it covers both the ImGui renderer smoke test and the whole resize sequence.
TEST_CASE("VkmEngine - drives a frame, and publishes/suspends/applies a window resize") {
    glfwInit();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(256, 256, "UnitTest", nullptr, nullptr);
    if (window == nullptr) {
        MESSAGE("Skipping: glfwCreateWindow failed (no display server available in this environment).");
        glfwTerminate();
        return;
    }

    vkm::VkmEngine engine(new vkm::VkmDriverVulkan());
    REQUIRE(engine.initializeEngine(new NullAppDelegate(), vkm::VkmEngineLaunchOptions{ .enableValidationLayer = true }));

    vkm::VkmInitResult initResult = engine.initializeBackendDriver();
    if (initResult.code == vkm::VkmInitResultCode::HardwareUnsupported) {
        MESSAGE("Skipping: " << initResult.reason);
        glfwDestroyWindow(window);
        glfwTerminate();
        return;
    }
    REQUIRE_MESSAGE(initResult.code == vkm::VkmInitResultCode::Success, initResult.reason);

    // isImGuiWindow = true so the engine creates and binds its ImGui renderer to this window
    // (when built with ImGui). That also puts every loopInner() below on the single-window path,
    // where a skipped frame is what discardFrame() has to close out.
    engine.addSwapChain(vkm::VkmWindowInfo{ 256, 256, "UnitTest", window }, /*isImGuiWindow=*/true);
    vkm::VkmSwapChainBase* swapChain = engine.getMainSwapChain();
    REQUIRE(swapChain != nullptr);

    // The ImGui renderer initializes and a plain frame ticks through.
    CHECK_NOTHROW(engine.loopInner(0.001));

    const glm::uvec2 originalExtent = swapChain->getExtent();

    // Deliberately no SUBCASEs: doctest re-runs the whole test body per subcase, and
    // initializeEngine() registers process-wide loggers that cannot be registered twice. The
    // scenarios below are sequential anyway -- each leaves the swapchain in a known state.

    // Actually resize the window rather than only publishing a number: on Vulkan the surface's
    // currentExtent is what the rebuilt swapchain adopts, so a size the window never took would
    // simply be ignored. Returns the resulting framebuffer size in pixels.
    const auto resizeWindowTo = [&](int width, int height) {
        glfwSetWindowSize(window, width, height);
        glfwPollEvents();
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        return glm::uvec2((uint32_t)framebufferWidth, (uint32_t)framebufferHeight);
    };

    const glm::uvec2 resizedExtent = resizeWindowTo(384, 320);
    if (resizedExtent == originalExtent) {
        MESSAGE("Skipping: the window manager did not honour glfwSetWindowSize.");
        engine.destroy();
        glfwDestroyWindow(window);
        glfwTerminate();
        return;
    }

    // --- nothing is rebuilt while a live resize is in progress ---
    engine.onWindowLiveResizeChanged(window, true);
    engine.onWindowResized(window, resizedExtent.x, resizedExtent.y);

    CHECK(engine.isWindowRenderingSuspended(0));
    CHECK_NOTHROW(engine.loopInner(0.016));
    CHECK(swapChain->getExtent() == originalExtent);

    // Ending the resize is what releases it, and one frame is enough to apply it.
    engine.onWindowLiveResizeChanged(window, false);
    CHECK_FALSE(engine.isWindowRenderingSuspended(0));
    CHECK_NOTHROW(engine.loopInner(0.032));
    CHECK(swapChain->getExtent() == resizedExtent);

    // --- a size published between frames is applied on the next one ---
    const glm::uvec2 restoredExtent = resizeWindowTo(256, 256);
    engine.onWindowResized(window, restoredExtent.x, restoredExtent.y);
    // Not applied yet: the window thread only publishes, the frame loop consumes.
    CHECK(swapChain->getExtent() == resizedExtent);

    CHECK_NOTHROW(engine.loopInner(0.048));
    CHECK(swapChain->getExtent() == restoredExtent);

    // A frame with nothing pending leaves it alone.
    CHECK_NOTHROW(engine.loopInner(0.064));
    CHECK(swapChain->getExtent() == restoredExtent);

    // --- a minimized window reports suspended and recovers on restore ---
    engine.onWindowResized(window, 0u, 0u);
    CHECK_NOTHROW(engine.loopInner(0.080));
    CHECK(swapChain->getExtent() == glm::uvec2(0u, 0u));
    CHECK(engine.isWindowRenderingSuspended(0));

    // Further frames must not touch the torn-down swapchain.
    CHECK_NOTHROW(engine.loopInner(0.096));

    engine.onWindowResized(window, restoredExtent.x, restoredExtent.y);
    CHECK_NOTHROW(engine.loopInner(0.112));
    CHECK(swapChain->getExtent() == restoredExtent);
    CHECK_FALSE(engine.isWindowRenderingSuspended(0));

    engine.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
}

#endif // VKM_USE_VULKAN_API

// ---------------------------------------------------------------------------
// WebGPU headless driver tests
// ---------------------------------------------------------------------------

#ifdef VKM_USE_WEBGPU_API

struct WebGPUDriverFixture {
    vkm::VkmDriverWebGPU* driver = nullptr;
    vkm::VkmInitResult initResult;
    WebGPUDriverFixture() {
        vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
        driver = new vkm::VkmDriverWebGPU();
        initResult = driver->initialize(&opts);
    }
    ~WebGPUDriverFixture() { delete driver; }
};

TEST_CASE("VkmDriverWebGPU - initialization succeeds") {
    WebGPUDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    SUBCASE("WGPUInstance is exposed and non-null") {
        CHECK(f.driver->getInstance() != nullptr);
    }
    SUBCASE("WGPUAdapter is exposed and non-null") {
        CHECK(f.driver->getAdapter() != nullptr);
    }
    SUBCASE("WGPUDevice is exposed and non-null") {
        CHECK(f.driver->getDevice() != nullptr);
    }
    SUBCASE("WGPUQueue is exposed and non-null") {
        CHECK(f.driver->getQueue() != nullptr);
    }
    SUBCASE("render resource pool is available") {
        CHECK(f.driver->getRenderResourcePool() != nullptr);
    }
    SUBCASE("graphics command queue is created") {
        CHECK(f.driver->getCommandQueue(vkm::VkmCommandQueueType::Graphics, 0) != nullptr);
    }
    SUBCASE("compute command queue is created") {
        CHECK(f.driver->getCommandQueue(vkm::VkmCommandQueueType::Compute, 0) != nullptr);
    }
    SUBCASE("driver capability flags are TextureUpload plus the adapter's timestamp support") {
        // TextureUpload without BindlessTextures is the distinguishing pair on this backend:
        // wgpuQueueWriteTexture gets pixels in, but WGSL has no array-of-handle type, so nothing
        // can index them from set 0 -- material textures arrive through descriptor set 3 instead.
        // TimestampQuery is adapter-dependent (VkmGpuProfiler's pool is only created when the
        // adapter offers the optional feature), so mask it out rather than assert a value that
        // changes with the machine the test runs on.
        constexpr uint32_t kTimestampQuery =
            static_cast<uint32_t>(vkm::VkmDriverCapabilityFlags::TimestampQuery);
        const uint32_t flags = static_cast<uint32_t>(f.driver->getDriverCapabilityFlags());
        CHECK((flags & ~kTimestampQuery) ==
              static_cast<uint32_t>(vkm::VkmDriverCapabilityFlags::TextureUpload));
        CHECK((flags & static_cast<uint32_t>(vkm::VkmDriverCapabilityFlags::BindlessTextures)) == 0u);
        // Spelled out rather than left to the equality above: WebGPU has no acceleration
        // structure or ray query in the API at all, so this is structural and not a property of
        // the adapter -- a future flag change should not be able to make it true by accident.
        CHECK((flags & static_cast<uint32_t>(vkm::VkmDriverCapabilityFlags::RayTracing)) == 0u);
    }
}

TEST_CASE("VkmDriverWebGPU - graphics queue exposes a valid WGPUQueue") {
    WebGPUDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);
    auto* queue = static_cast<vkm::VkmCommandQueueWebGPU*>(
        f.driver->getCommandQueue(vkm::VkmCommandQueueType::Graphics, 0));
    REQUIRE(queue != nullptr);
    CHECK(queue->getWGPUQueue() != nullptr);
}

#endif // VKM_USE_WEBGPU_API
