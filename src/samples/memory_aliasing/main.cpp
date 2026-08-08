// Copyright (c) 2025 Snowapril
//
// A benchmark for the engine's two memory-saving resource options, with both switchable at run
// time so the cost of each is a before/after measurement rather than a claim.
//
//   VkmResourceCreateInfo::Aliasable  — stage targets whose render-graph lifetimes do not
//                                       overlap share one block of memory.
//   VkmResourceCreateInfo::Transient  — the depth attachment nothing ever reads never reaches
//                                       device memory at all.
//
// The frame is a chain of full-screen passes:
//
//   Generate -> T0 ; Process -> T1 (reads T0) ; ... ; Present -> back buffer (reads T[N-1])
//
// Each stage target is written by one pass and read by the next, so its lifetime is two
// subgraphs wide and *alternating* stages never coexist: T0 is dead before T2 is born. With N
// targets the packer needs only two blocks' worth of memory instead of N, which the memory
// readout shows directly. All four stage passes share one depth attachment that no pass ever
// samples -- the textbook case for Transient.
//
// Toggling either option recreates the targets, because both are creation flags and an aliased
// texture's placement is frozen once chosen. That is also why the sample skips one frame after a
// toggle: an Aliasable texture has no memory until the render graph's next compile() has seen
// its declared lifetime, so the resource tables that name it cannot be built before then.

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include <cxxopts.hpp>

#if defined(VKM_ENABLE_IMGUI)
#include <imgui.h>
#endif

#include <vkm/base/common.h>
#include <vkm/platform/common/app_delegate.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_PLATFORM_WINDOWS)
#include <vkm/platform/windows/application.h>
#elif defined(VKM_PLATFORM_WASM)
#include <vkm/platform/wasm/application.h>
#elif defined(VKM_PLATFORM_LINUX)
#include <vkm/platform/linux/application.h>
#else
#include <vkm/platform/apple/application.h>
#endif

using namespace vkm;

namespace
{
    // Four is the smallest count that shows aliasing doing something non-trivial: with two, the
    // only pair overlaps and nothing can be shared; with four, T0/T2 and T1/T3 pair off and the
    // footprint halves.
    constexpr uint32_t kStageCount = 4;

    struct MemStageConstants
    {
        float _params[4]; // x = phase, y = stage index, z = stage count, w unused
    };

    VkmBuffer* createUniformBuffer(VkmDriverBase* driver, uint64_t size, const char* name)
    {
        VkmBufferInfo info{};
        info._flags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst));
        info._size = size;
        info._debugName = name;
        return driver->newBuffer(info);
    }

    std::string formatBytes(uint64_t bytes)
    {
        constexpr double kMiB = 1024.0 * 1024.0;
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.2f MiB", static_cast<double>(bytes) / kMiB);
        return buffer;
    }
}

class MemoryAliasingApplication : public AppDelegate
{
public:
    MemoryAliasingApplication(bool aliasingEnabled, bool transientDepthEnabled)
        : _aliasingEnabled(aliasingEnabled), _transientDepthEnabled(transientDepthEnabled)
    {
        // Explicitly, not via `{}` on the members: a value-initialized VkmResourceHandle has
        // id == 0, which isValid() reports as *true*, so the first rebuild would "release"
        // whatever really owns slot 0 -- the swapchain's first back buffer.
        _targets.fill(VKM_INVALID_RESOURCE_HANDLE);
        _stageConstants.fill(VKM_INVALID_RESOURCE_HANDLE);
    }
    ~MemoryAliasingApplication() override = default;

    void postDriverReady(VkmEngine* engine) override final
    {
        _engine = engine;
        VkmDriverBase* driver = engine->getDriver();

        VkmPipelineStateManager* manager = engine->getPipelineStateManager();
        std::string error;
        if (!manager->loadPipelineStatesFromDirectory(SAMPLE_DIR, SAMPLE_SHADER_CACHE_DIR,
                                                      VkmPipelineStateOrigin::User, &error))
        {
            VKM_DEBUG_ERROR(("Failed to load the memory_aliasing pipeline states: " + error).c_str());
            return;
        }
        _generatePipeline = manager->getPipelineState("mem_generate_pso", VkmPipelineStateOrigin::User);
        _processPipeline = manager->getPipelineState("mem_process_pso", VkmPipelineStateOrigin::User);
        _presentPipeline = manager->getPipelineState("mem_present_pso", VkmPipelineStateOrigin::User);
        VKM_ASSERT(_generatePipeline != nullptr && _processPipeline != nullptr && _presentPipeline != nullptr,
                   "Failed to load the memory_aliasing pipeline states");

        VkmSamplerInfo samplerInfo{};
        samplerInfo._debugName = "MemAliasSampler";
        VkmSampler* sampler = driver->newSampler(samplerInfo);
        _sampler = sampler != nullptr ? sampler->getHandle() : VKM_INVALID_RESOURCE_HANDLE;

        // One constant buffer per stage rather than one shared: every stage is recorded into the
        // same command buffer and they differ only by index, so a single buffer rewritten
        // per stage would race with itself.
        for (uint32_t stage = 0; stage < kStageCount; ++stage)
        {
            const std::string name = "MemAliasStageConstants" + std::to_string(stage);
            VkmBuffer* buffer = createUniformBuffer(driver, sizeof(MemStageConstants), name.c_str());
            _stageConstants[stage] = buffer != nullptr ? buffer->getHandle() : VKM_INVALID_RESOURCE_HANDLE;
        }
    }

    void preShutdown() override final
    {
        destroyTables();
        releaseTargets(/*deferred=*/false);
    }

    void update(const double deltaTime) override final
    {
        _phase += static_cast<float>(deltaTime);

        // A rolling mean rather than the instantaneous value: the whole point is comparing two
        // configurations, and a number that jitters by 30% frame to frame cannot be compared.
// Anything past a second is startup or a stall, not a frame; one such sample poisons the
        // mean for the whole window and makes the two configurations incomparable.
        if (deltaTime > 0.0 && deltaTime < 1.0)
        {
            _frameTimeSamples[_frameTimeCursor] = static_cast<float>(deltaTime * 1000.0);
            _frameTimeCursor = (_frameTimeCursor + 1) % kFrameTimeSampleCount;
            _frameTimeSampleTotal = std::min(_frameTimeSampleTotal + 1, kFrameTimeSampleCount);
        }

        drawUi();

        // Also reported to the console on a fixed cadence: the whole point of the sample is
        // comparing two runs, and a number only visible in an overlay cannot be diffed, scripted
        // or pasted into a review.
        _reportTimer += deltaTime;
        if (_reportTimer >= kReportIntervalSeconds)
        {
            _reportTimer = 0.0;
            reportToConsole();
        }
    }

    void reportToConsole() const
    {
        VkmDriverBase* driver = _engine->getDriver();
        const VkmResourceCategoryUsage textures =
            driver->getRenderResourcePool()->getCategoryMemoryUsage(VkmResourceType::Texture);
        const VkmGpuMemoryStats gpu = driver->getGpuMemoryStats();

        VKM_DEBUG_INFO(fmt::format(
            "[memory_aliasing] aliasing={} transientDepth={} | textures requested {} allocated {} | "
            "device {} pool {} | {:.3f} ms",
            _aliasingEnabled ? "on" : "off", _transientDepthEnabled ? "on" : "off",
            formatBytes(textures.totalRequestedBytes), formatBytes(textures.totalAllocatedBytes),
            gpu._hasDeviceStats ? formatBytes(gpu._deviceAllocatedBytes) : std::string("n/a"),
            gpu._hasPoolStats ? formatBytes(gpu._poolReservedBytes) : std::string("n/a"),
            averageFrameTimeMs()).c_str());
    }

    void render(uint32_t windowIndex, VkmRenderGraph* renderGraph, VkmResourceHandle backBuffer) override final
    {
        const glm::uvec2 extent = _engine->getSwapChain(windowIndex)->getExtent();
        if (extent.x == 0 || extent.y == 0)
        {
            return;
        }

        if (_pendingRebuild || extent != _extent)
        {
            _extent = extent;
            rebuildTargets();
        }

        // An Aliasable target has no memory until the first compile() that declares its lifetime,
        // so the tables naming it cannot exist yet. Clear the back buffer for that one frame
        // rather than drawing with half-built state.
        if (!tablesComplete())
        {
            if (targetsPlaced())
            {
                buildTables();
            }
            if (!tablesComplete())
            {
                recordClear(renderGraph, backBuffer);
                return;
            }
        }

        uploadStageConstants();

        // Generate -> T0, with the shared transient depth attached.
        recordStage(renderGraph, "MemAliasGenerate", _generatePipeline, _tables[0], _targets[0],
                    /*attachDepth=*/true);

        // Process -> T[i], reading T[i-1]. The read is what keeps consecutive stages overlapping
        // and alternating ones disjoint.
        for (uint32_t stage = 1; stage < kStageCount; ++stage)
        {
            const std::string name = "MemAliasProcess" + std::to_string(stage);
            recordStage(renderGraph, name.c_str(), _processPipeline, _tables[stage], _targets[stage],
                        /*attachDepth=*/true, /*inputTarget=*/_targets[stage - 1]);
        }

        // Present -> back buffer, reading the last stage. No depth: the back buffer has none.
        VkmFrameBufferDescriptor presentFb = makeStageFb(backBuffer, /*attachDepth=*/false);
        VkmRenderGraphicsSubGraph* presentSubGraph = renderGraph->beginGraphicsSubGraph(presentFb, "MemAliasPresent");
        declareAliased(presentSubGraph, _targets[kStageCount - 1]);
        VkmPipelineStateBase* presentPipeline = _presentPipeline;
        VkmResourceTableBase* presentTable = _presentTable;
        presentSubGraph->setRenderCallback([presentPipeline, presentTable](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(presentPipeline);
            commandBuffer->bindResourceTable(presentTable);
            commandBuffer->draw(3, 1, 0, 0);
        });
    }

    const char* getAppName() const override final { return "MemoryAliasingApplication"; }

private:
    static constexpr uint32_t kFrameTimeSampleCount = 120;
    static constexpr double kReportIntervalSeconds = 3.0;

    bool targetsPlaced() const
    {
        VkmRenderResourcePool* pool = _engine->getDriver()->getRenderResourcePool();
        for (uint32_t stage = 0; stage < kStageCount; ++stage)
        {
            VkmTexture* texture = pool->getResource<VkmTexture>(_targets[stage]);
            if (texture == nullptr || !texture->isAliasPlaced())
            {
                return false;
            }
        }
        return true;
    }

    bool tablesComplete() const
    {
        if (_presentTable == nullptr)
        {
            return false;
        }
        return std::all_of(_tables.begin(), _tables.end(), [](VkmResourceTableBase* t) { return t != nullptr; });
    }

    // Only an Aliasable texture needs declaring; a plain one has no lifetime for the graph to
    // track. Calling it unconditionally would just log warnings when aliasing is off.
    void declareAliased(VkmRenderSubGraph* subGraph, VkmResourceHandle handle)
    {
        subGraph->addReferencedResource(handle);
        if (_aliasingEnabled)
        {
            subGraph->addAliasedResource(handle);
        }
    }

    VkmFrameBufferDescriptor makeStageFb(VkmResourceHandle target, bool attachDepth) const
    {
        VkmFrameBufferDescriptor fb{};
        fb._width = _extent.x;
        fb._height = _extent.y;
        fb._renderPass._colorAttachmentCount = 1;
        fb._renderPass._colorAttachments[0]._attachmentId = 0;
        // DontCare, not Clear: every stage overwrites the whole target, and an aliased target's
        // previous contents belong to another texture anyway.
        fb._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::DontCare;
        fb._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
        fb._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;
        fb._colorAttachments[0] = target;

        if (attachDepth && _depthTarget.isValid())
        {
            VkmDepthStencilAttachmentDescriptor depth{};
            depth._attachmentId = 1;
            // Cleared per pass and never stored: nothing reads it, which is exactly what makes
            // it a Transient candidate. Clear rather than DontCare so the depth test is
            // deterministic -- a Transient attachment may not be Loaded, but may be Cleared.
            depth._loadAction = VkmLoadAction::Clear;
            depth._storeAction = VkmStoreAction::DontCare;
            depth._clearDepth = 1.0f;
            depth._clearStencil = 0;
            fb._renderPass._depthStencilAttachment = depth;
            fb._depthStencilAttachment = _depthTarget;
        }
        return fb;
    }

    void recordStage(VkmRenderGraph* renderGraph, const char* name, VkmPipelineStateBase* pipeline,
                     VkmResourceTableBase* table, VkmResourceHandle target, bool attachDepth,
                     VkmResourceHandle inputTarget = VKM_INVALID_RESOURCE_HANDLE)
    {
        VkmRenderGraphicsSubGraph* subGraph =
            renderGraph->beginGraphicsSubGraph(makeStageFb(target, attachDepth), name);
        declareAliased(subGraph, target);
        if (inputTarget.isValid())
        {
            // The read half of the lifetime. Without this the graph would think the input died
            // at the pass that wrote it and could hand its bytes to this pass's own output.
            declareAliased(subGraph, inputTarget);
        }
        subGraph->setRenderCallback([pipeline, table](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pipeline);
            commandBuffer->bindResourceTable(table);
            commandBuffer->draw(3, 1, 0, 0);
        });
    }

    void recordClear(VkmRenderGraph* renderGraph, VkmResourceHandle backBuffer)
    {
        VkmFrameBufferDescriptor fb{};
        fb._width = _extent.x;
        fb._height = _extent.y;
        fb._renderPass._colorAttachmentCount = 1;
        fb._renderPass._colorAttachments[0]._attachmentId = 0;
        fb._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::Clear;
        fb._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
        fb._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;
        fb._colorAttachments[0] = backBuffer;
        renderGraph->beginGraphicsSubGraph(fb, "MemAliasPlacementGate");
    }

    void uploadStageConstants()
    {
        VkmDriverBase* driver = _engine->getDriver();
        for (uint32_t stage = 0; stage < kStageCount; ++stage)
        {
            MemStageConstants constants{};
            constants._params[0] = _phase;
            constants._params[1] = static_cast<float>(stage);
            constants._params[2] = static_cast<float>(kStageCount);
            driver->uploadToBuffer(_stageConstants[stage], &constants, sizeof(constants));
        }
    }

    void rebuildTargets()
    {
        destroyTables();
        releaseTargets(/*deferred=*/true);
        createTargets();
        _pendingRebuild = false;
    }

    void createTargets()
    {
        VkmDriverBase* driver = _engine->getDriver();

        VkmResourceCreateInfo colorFlags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowColorAttachment) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead));
        if (_aliasingEnabled)
        {
            colorFlags = colorFlags | VkmResourceCreateInfo::Aliasable;
        }

        for (uint32_t stage = 0; stage < kStageCount; ++stage)
        {
            const std::string name = "MemAliasStage" + std::to_string(stage);
            VkmTextureInfo info{};
            info._flags = colorFlags;
            info._extent = glm::uvec3(_extent, 1);
            info._numMipLevels = 1;
            info._numArrayLayers = 1;
            info._format = driver->getSwapChainColorFormat();
            info._debugName = name.c_str();
            VkmTexture* texture = driver->newTexture(info);
            _targets[stage] = texture != nullptr ? texture->getHandle() : VKM_INVALID_RESOURCE_HANDLE;
        }

        // Attachment usage only: nothing samples it, nothing copies out of it. That is both what
        // makes Transient legal here and what makes it worth having.
        VkmResourceCreateInfo depthFlags = VkmResourceCreateInfo::AllowDepthStencilAttachment;
        if (_transientDepthEnabled)
        {
            depthFlags = depthFlags | VkmResourceCreateInfo::Transient;
        }
        VkmTextureInfo depthInfo{};
        depthInfo._flags = depthFlags;
        depthInfo._extent = glm::uvec3(_extent, 1);
        depthInfo._numMipLevels = 1;
        depthInfo._numArrayLayers = 1;
        depthInfo._format = VkmFormat::D32_SFLOAT;
        depthInfo._debugName = "MemAliasDepth";
        VkmTexture* depthTexture = driver->newTexture(depthInfo);
        _depthTarget = depthTexture != nullptr ? depthTexture->getHandle() : VKM_INVALID_RESOURCE_HANDLE;
    }

    void releaseTargets(bool deferred)
    {
        VkmDriverBase* driver = _engine->getDriver();
        const auto release = [driver, deferred](VkmResourceHandle& handle) {
            if (!handle.isValid())
            {
                return;
            }
            if (deferred)
            {
                // Frames still in flight may be reading these, and for an aliased target the
                // range must not return to the packer until the GPU is done with it.
                driver->getDeferredReclaimer()->requestRelease(handle);
            }
            else
            {
                driver->getRenderResourcePool()->releaseResource(handle);
            }
            handle = VKM_INVALID_RESOURCE_HANDLE;
        };

        for (uint32_t stage = 0; stage < kStageCount; ++stage)
        {
            release(_targets[stage]);
        }
        release(_depthTarget);
    }

    void buildTables()
    {
        VkmDriverBase* driver = _engine->getDriver();
        std::string error;

        _tables[0] = driver->newResourceTable(_generatePipeline, VkmResourceSetKind::PerPass,
                                              {{0, _stageConstants[0]}}, &error);
        for (uint32_t stage = 1; stage < kStageCount; ++stage)
        {
            _tables[stage] = driver->newResourceTable(
                _processPipeline, VkmResourceSetKind::PerPass,
                {{0, _targets[stage - 1]}, {1, _sampler}, {2, _stageConstants[stage]}}, &error);
        }
        _presentTable = driver->newResourceTable(_presentPipeline, VkmResourceSetKind::PerPass,
                                                 {{0, _targets[kStageCount - 1]}, {1, _sampler}}, &error);

        if (!tablesComplete())
        {
            VKM_DEBUG_ERROR(("Failed to build the memory_aliasing per-pass tables: " + error).c_str());
        }
    }

    void destroyTables()
    {
        const auto destroy = [](VkmResourceTableBase*& table) {
            if (table != nullptr)
            {
                table->destroy();
                delete table;
                table = nullptr;
            }
        };
        for (VkmResourceTableBase*& table : _tables)
        {
            destroy(table);
        }
        destroy(_presentTable);
    }

    void drawUi()
    {
#if defined(VKM_ENABLE_IMGUI)
        VkmDriverBase* driver = _engine->getDriver();
        VkmRenderResourcePool* pool = driver->getRenderResourcePool();

        ImGui::Begin("Memory aliasing");

        if (ImGui::Checkbox("Alias stage targets", &_aliasingEnabled))
        {
            _pendingRebuild = true;
        }
        if (ImGui::Checkbox("Transient depth", &_transientDepthEnabled))
        {
            _pendingRebuild = true;
        }
        ImGui::TextWrapped("Both are creation flags, so toggling recreates the targets and skips one frame "
                           "while the render graph places them.");

        ImGui::Separator();
        ImGui::Text("%u stage targets at %ux%u", kStageCount, _extent.x, _extent.y);

        // "Requested" is what the caller asked for and never changes; "allocated" is what the
        // engine tracks as really committed, and an aliased or transient texture reports zero
        // there because its bytes are counted once as a heap block instead.
        const VkmResourceCategoryUsage textures = pool->getCategoryMemoryUsage(VkmResourceType::Texture);
        ImGui::Text("Textures requested: %s", formatBytes(textures.totalRequestedBytes).c_str());
        ImGui::Text("Textures allocated: %s", formatBytes(textures.totalAllocatedBytes).c_str());

        const VkmGpuMemoryStats gpu = driver->getGpuMemoryStats();
        if (gpu._hasDeviceStats)
        {
            ImGui::Text("Device allocated:   %s", formatBytes(gpu._deviceAllocatedBytes).c_str());
        }
        if (gpu._hasPoolStats)
        {
            ImGui::Text("Pool reserved:      %s", formatBytes(gpu._poolReservedBytes).c_str());
        }

        ImGui::Separator();
        ImGui::Text("Frame: %.3f ms (%.0f fps)", averageFrameTimeMs(),
                    averageFrameTimeMs() > 0.0f ? 1000.0f / averageFrameTimeMs() : 0.0f);
        ImGui::TextWrapped("Aliasing trades memory for ordering: each acquisition inserts a barrier, so a "
                           "frame-time cost here is the feature working, not a bug.");

        ImGui::End();
#endif
    }

    float averageFrameTimeMs() const
    {
        if (_frameTimeSampleTotal == 0)
        {
            return 0.0f;
        }
        float total = 0.0f;
        for (uint32_t i = 0; i < _frameTimeSampleTotal; ++i)
        {
            total += _frameTimeSamples[i];
        }
        return total / static_cast<float>(_frameTimeSampleTotal);
    }

private:
    VkmEngine* _engine{nullptr};

    VkmPipelineStateBase* _generatePipeline{nullptr};
    VkmPipelineStateBase* _processPipeline{nullptr};
    VkmPipelineStateBase* _presentPipeline{nullptr};

    std::array<VkmResourceHandle, kStageCount> _targets{};
    std::array<VkmResourceHandle, kStageCount> _stageConstants{};
    std::array<VkmResourceTableBase*, kStageCount> _tables{};
    VkmResourceTableBase* _presentTable{nullptr};
    VkmResourceHandle _depthTarget{VKM_INVALID_RESOURCE_HANDLE};
    VkmResourceHandle _sampler{VKM_INVALID_RESOURCE_HANDLE};

    glm::uvec2 _extent{0, 0};
    float _phase{0.0f};
    bool _aliasingEnabled{true};
    bool _transientDepthEnabled{true};
    bool _pendingRebuild{true};
    double _reportTimer{0.0};

    std::array<float, kFrameTimeSampleCount> _frameTimeSamples{};
    uint32_t _frameTimeCursor{0};
    uint32_t _frameTimeSampleTotal{0};
};

int main(int argc, char* argv[])
{
    cxxopts::Options options("memory_aliasing", "Measure VkmResourceCreateInfo::Aliasable and ::Transient");
    options.allow_unrecognised_options();
    options.add_options()
        ("no-aliasing", "Start with stage-target aliasing off")
        ("no-transient-depth", "Start with the transient depth attachment off")
        ("h,help", "Print usage");

    const cxxopts::ParseResult parsed = options.parse(argc, argv);
    if (parsed.count("help") != 0)
    {
        std::printf("%s\n", options.help().c_str());
        return 0;
    }

    VkmApplication app;
    const int ret = app.entryPoint(new MemoryAliasingApplication(parsed.count("no-aliasing") == 0,
                                                                 parsed.count("no-transient-depth") == 0),
                                   argc, argv);
    app.destroy();
    return ret;
}
