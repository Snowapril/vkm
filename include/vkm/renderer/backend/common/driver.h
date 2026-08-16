// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/platform/common/window.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/engine.h>

#include <array>
#include <vector>
#include <memory>
#include <string>

namespace vkm
{
    struct VkmPipelineStateDescriptor;
    class VkmTexture;
    class VkmBuffer;
    class VkmStagingBuffer;
    class VkmSampler;
    class VkmTextureView;
    class VkmBufferView;
    class VkmAccelerationStructure;
    class VkmUpscalerBase;
    struct VkmUpscalerDescriptor;
    struct VkmAccelerationStructureInfo;
    class VkmSwapChainBase;
    class VkmCommandQueueBase;
    class VkmCommandDispatcher;
    class VkmRenderResourcePool;
    class VkmAliasedMemoryHeap;
    class VkmPipelineStateBase;
    class VkmDeferredResourceReclaimer;
    class VkmGpuCrashHandler;
    class VkmGpuProfiler;
    class VkmCommandBufferBase;
    class VkmBindlessResourceManagerBase;
    class VkmFrameConstantManagerBase;

    enum class VkmDriverCapabilityFlags : uint32_t
    {
        None                    = 0x000000000,
        CommandBufferReusable   = 0x00000001,
        // Backend implements copyTexture (texture-to-texture), so render graph capture can
        // snapshot texture contents. (copyTextureToBuffer/readbackTexture are cross-backend
        // and not gated by this flag.)
        TextureContentCapture   = 0x00000002,
        // Backend can get pixels into a texture at all, by whichever route: a
        // copyBufferToTexture staging copy (Vulkan, Metal) or a queue write (WebGPU).
        TextureUpload           = 0x00000004,
        // Backend can write a texture's memory from the CPU, so uploadToTexture can skip the
        // staging buffer and the queue submit. The per-device precondition only: a texture takes
        // that path when VkmTexture::isHostWritable also says its own memory allows it.
        TextureHostCopy         = 0x00000008,
        // Buffers can report an address in the GPU's address space
        // (VkmBuffer/VkmStagingBuffer::getGPUVirtualAddress). Native on Metal; on Vulkan it needs
        // VkPhysicalDeviceVulkan12Features::bufferDeviceAddress. WebGPU has no such concept, and
        // neither does a Vulkan driver without that feature -- both report 0 rather than failing.
        BufferDeviceAddress     = 0x00000010,
        // Backend can write GPU timestamps into a pool of slots and read them back, which is what
        // VkmGpuProfiler needs to time render graph subgraphs. Set by each backend from
        // initializeGpuTimestampPool(), so it is only meaningful after driver initialization.
        TimestampQuery          = 0x00000020,
        /*
        * Backend has the set-0 bindless texture array, so registerTexture returns a slot a shader
        * can index. Separate from TextureUpload because WebGPU has one and not the other: WGSL has
        * no array-of-handle type, so pixels upload there but nothing can index them, and material
        * textures reach a WebGPU shader through descriptor set 3 instead. The distinction tells
        * "this backend has no such array" apart from "the array is exhausted".
        */
        BindlessTextures        = 0x00000040,
        /*
        * Backend can build acceleration structures and traverse them from a shader ray query. One
        * flag rather than three because the pieces are useless apart: Vulkan requests
        * VK_KHR_acceleration_structure, VK_KHR_ray_query and VK_KHR_deferred_host_operations as a
        * set, and Metal 4 answers through MTLDevice.supportsRaytracing.
        * Ray tracing *pipelines* are outside this: the engine casts rays from compute shaders, so
        * it needs no shader binding tables. A backend that gains them wants its own flag.
        * Absent on WebGPU and on MoltenVK, so on macOS the Vulkan backend reports no ray tracing
        * while the Metal backend reports it -- hence a runtime capability rather than an #ifdef.
        */
        RayTracing              = 0x00000080,
        /*
        * Backend can create a VkmUpscalerBase (see upscaler.h): MetalFX on Metal, FSR on Vulkan
        * where the FidelityFX SDK is built in. A runtime capability for the same reason
        * RayTracing is -- on macOS the Metal backend reports it while MoltenVK does not -- and
        * clear on WebGPU, which has no upscaler library at all.
        */
        TemporalUpscaling       = 0x00000100,
    };

    inline VkmDriverCapabilityFlags operator|(VkmDriverCapabilityFlags lhs, VkmDriverCapabilityFlags rhs)
    {
        return static_cast<VkmDriverCapabilityFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    inline uint32_t operator&(VkmDriverCapabilityFlags lhs, VkmDriverCapabilityFlags rhs)
    {
        return static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs);
    }

    // CPU-side pixel data returned by VkmDriverBase::readbackTexture(). Row-major,
    // tightly packed, `channels` bytes per pixel in the texture's native channel order
    // (e.g. BGRA for BGRA8_UNORM).
    struct VkmTextureReadbackResult
    {
        std::vector<uint8_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t channels = 0;
    };

    enum class VkmInitResultCode
    {
        Success,
        HardwareUnsupported,  // environment/hardware cannot satisfy this backend's requirements
        Failed,               // any other initialization error - a real bug, must not be skipped
    };

    struct VkmInitResult
    {
        VkmInitResultCode code = VkmInitResultCode::Success;
        std::string       reason;
    };

    /*
    * @brief renderer backend driver base class
    * @details manage whole engine lifecycle and drive the render driver and other modules
    */
    class VkmDriverBase
    {
    public:
        VkmDriverBase();
        virtual ~VkmDriverBase();

        /*
        * @brief initialize each graphics api setup and create necessary resources
        */
        VkmInitResult initialize(const VkmEngineLaunchOptions* options);

        /*
        * @brief create command queues for each necessary needs
        */
        bool setUpPredefinedCommandQueues();

        /*
        * @brief destroy all resources and clean up
        */
        void destroy();

        /*
         * @brief Create texture with existing rhi external handle
         * @param info texture info
         */
        VkmTexture* newTexture(const VkmTextureInfo& info);

        /*
         * @brief Create buffer with the given buffer info
         */
        VkmBuffer* newBuffer(const VkmBufferInfo& info);

        /*
         * @brief Create staging buffer with the given staging buffer info
         */
        VkmStagingBuffer* newStagingBuffer(const VkmStagingBufferInfo& info);

        /*
         * @brief Synchronous GPU texture -> CPU pixels readback of mip 0 of one array layer of an
         * uncompressed color texture.
         * @details Goes through a transient readback staging buffer and a one-off command buffer on
         * the Graphics queue, and blocks until the copy completes. Intended for tests and
         * debugging, not per-frame use.
         * @param textureHandle Texture to read back.
         * @param arrayLayer Array layer to read.
         * @return The pixels, or an empty result on failure.
         */
        VkmTextureReadbackResult readbackTexture(VkmResourceHandle textureHandle, uint32_t arrayLayer = 0);

        /*
         * @brief Synchronously uploads bytes into a buffer.
         * @details The staging path blocks until the GPU copy completes, so this is for setup-time
         * uploads (e.g. postDriverReady), not per-frame streaming. It uses a transient staging
         * buffer and a one-off command buffer on the Graphics queue; this engine has no dedicated
         * Transfer queue.
         * @param dstBuffer Destination buffer.
         * @param data Source bytes.
         * @param size Number of bytes to upload.
         * @param dstOffset Byte offset into the destination.
         * @param mode How the bytes get there. Auto takes the direct CPU write when the buffer was
         * created with VkmMemoryAccessHint::HostWrite, and the staging copy otherwise.
         * @return False if the upload failed.
         */
        bool uploadToBuffer(VkmResourceHandle dstBuffer, const void* data, uint64_t size, uint64_t dstOffset = 0,
                             VkmResourceUploadMode mode = VkmResourceUploadMode::Auto);

        /*
         * @brief Synchronously uploads tightly-packed pixels into one mip level of one array layer.
         * @details The texture counterpart of uploadToBuffer, with the same transient-staging,
         * one-off-command-buffer, blocking shape and the same setup-time-only intent. A cubemap is
         * six calls, one per face, with arrayLayer running over VkmTextureInfo's +X, -X, +Y, -Y,
         * +Z, -Z order. The texture is sampleable once this returns.
         * @param dstTexture Destination texture.
         * @param data Source pixels, tightly packed.
         * @param size Number of bytes to upload.
         * @param mipLevel Mip level to write.
         * @param arrayLayer Array layer to write.
         * @param mode How the pixels get there. Auto takes the direct CPU write when the texture's
         * memory allows it -- that path does no queue work and does not block -- and the staging
         * copy otherwise.
         * @return False if the upload failed.
         */
        bool uploadToTexture(VkmResourceHandle dstTexture, const void* data, uint64_t size,
                             uint32_t mipLevel = 0, uint32_t arrayLayer = 0,
                             VkmResourceUploadMode mode = VkmResourceUploadMode::Auto);

        /*
         * @brief Create sampler with the given sampler info
         */
        VkmSampler* newSampler(const VkmSamplerInfo& info);

        /*
        * @brief Creates and builds an acceleration structure.
        * @details Builds synchronously, through a one-off command buffer that is submitted and
        * waited on -- the same shape uploadToBuffer has. A per-frame rebuild or refit needs a
        * recorded build instead.
        * @param info Structure description.
        * @return The structure, or null with an error logged on a backend whose capability flags
        * lack VkmDriverCapabilityFlags::RayTracing. Check the flag rather than the result where the
        * absence is expected.
        */
        VkmAccelerationStructure* newAccelerationStructure(const VkmAccelerationStructureInfo& info);

        /*
        * @brief Creates a temporal upscaler for a fixed render/display extent pair.
        * @details The caller owns the result: destroy() then delete, once no in-flight frame can
        * still be using it -- the same lifetime contract as newResourceTable. A resize means a
        * new upscaler.
        * @param descriptor Extents and formats; see upscaler.h.
        * @return The upscaler, or null with an error logged on a backend whose capability flags
        * lack VkmDriverCapabilityFlags::TemporalUpscaling. Check the flag rather than the result
        * where the absence is expected.
        */
        VkmUpscalerBase* newUpscaler(const VkmUpscalerDescriptor& descriptor);

        /*
        * @brief Create swapchain with window info
        */
        VkmSwapChainBase* newSwapChain();

        /*
        * @brief Create a backend pipeline state object from a fully-resolved descriptor
        * @param desc fully-resolved pipeline state descriptor (already option-expanded)
        * @param shaderCacheDir directory containing this descriptor's .vfcache files
        */
        VkmPipelineStateBase* newPipelineState(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError = nullptr);

        /*
        * @brief Builds the resources `pipelineState` declared for one PSO-declared set: set 2
        * (VkmResourceSetKind::PerPass) or set 3 (PerDraw).
        * @details The caller owns the result: destroy() then delete, once no in-flight frame can
        * still be using it.
        * @param pipelineState Pipeline whose declaration the entries must cover.
        * @param kind Which set to build.
        * @param entries Resources to bind, one per declared binding.
        * @param outError Receives a message when the table cannot be built. May be null.
        * @return The table, or nullptr if the pipeline declares nothing for that set, if `entries`
        * does not exactly cover the declaration, or if a resource is of the wrong kind.
        */
        VkmResourceTableBase* newResourceTable(const VkmPipelineStateBase* pipelineState,
                                               VkmResourceSetKind kind,
                                               const std::vector<VkmTableResourceEntry>& entries,
                                               std::string* outError = nullptr);

        /*
        * @brief Replaces every VkmFormat::Swapchain color-format sentinel in a descriptor with the
        * concrete swapchain format.
        * @details The format converters (getMTLPixelFormat/toVkFormat/...) must never see
        * VkmFormat::Swapchain, so every path that hands a descriptor to a backend must run this
        * first. newPipelineState() and VkmPipelineStateManager's reload path both do.
        * @param desc Descriptor to resolve in place.
        * @param outError Receives a message when a sentinel cannot be resolved. May be null.
        * @return False if the descriptor could not be resolved.
        */
        bool resolveSwapChainFormats(VkmPipelineStateDescriptor& desc, std::string* outError = nullptr) const;

        /*
        * @brief Blocks until every command queue of this driver has finished all submitted work.
        * @details Used by the pipeline-state reload path, which destroys backend pipeline objects
        * synchronously rather than through the deferred reclaimer. Virtual because waiting on each
        * queue's timeline is not, on every backend, the same as the API considering the device
        * idle -- see VkmDriverVulkan::waitIdle.
        * @param timeoutMs Milliseconds to wait before giving up.
        */
        virtual void waitIdle(const uint64_t timeoutMs = UINT64_MAX);

        /*
        * @brief get driver capability flags
        */
        inline VkmDriverCapabilityFlags getDriverCapabilityFlags() const { return _driverCapabilityFlags; }

        /*
        * @brief Whether the CPU and GPU share one memory pool, so a texture placed in
        * CPU-writable memory costs the GPU nothing to sample.
        * @details Set by each backend during initializeInner (Metal: hasUnifiedMemory; Vulkan: a
        * DEVICE_LOCAL|HOST_VISIBLE memory type exists), and gates the host-copy texture upload
        * path. False on backends that never checked.
        */
        inline bool hasUnifiedMemory() const { return _hasUnifiedMemory; }

        /*
        * @brief get one of the driver's command queues
        */
        inline VkmCommandQueueBase* getCommandQueue(const VkmCommandQueueType queueType, const uint32_t commandQueueIndex) const
        {
            VKM_ASSERT(queueType < VkmCommandQueueType::Count, "Invalid command queue type");
            VKM_ASSERT(commandQueueIndex < _commandQueues[(uint8_t)queueType].size(), "Invalid command queue index");
            return _commandQueues[(uint8_t)queueType][commandQueueIndex];
        }

        /*
        * @brief get render resource pool
        */
        inline VkmRenderResourcePool* getRenderResourcePool() const { return _renderResourcePool.get(); }

        /*
        * @brief get the engine-global bindless resource manager (set 0 convention -- see
        * common/bindless_resource_manager.h).
        * @details Created by each backend driver during initialization: Vulkan in initializeInner,
        * since pipeline layouts need the set layout; Metal and WebGPU in postInitializeInner, since
        * they need the resource pool and queues.
        */
        inline VkmBindlessResourceManagerBase* getBindlessResourceManager() const { return _bindlessResourceManager.get(); }

        /*
        * @brief get the engine-global per-frame constant manager (set 1 convention -- see
        * common/frame_constants.h).
        * @details Created by each backend driver during initialization, next to the bindless
        * manager. VkmEngine::render() writes one frame slot through it per frame, and every
        * pipeline bind publishes that slot.
        */
        inline VkmFrameConstantManagerBase* getFrameConstantManager() const { return _frameConstantManager.get(); }

        /*
        * @brief get deferred resource reclaimer; VkmRenderGraph drives its per-frame pollOnce()
        * fallback on WASM through this accessor.
        */
        inline VkmDeferredResourceReclaimer* getDeferredReclaimer() const { return _deferredReclaimer.get(); }

        /*
        * @brief get the launch options this driver was initialized with.
        * @details Returns DEFAULT_ENGINE_LAUNCH_OPTIONS when initialize() was called with a null
        * options pointer, as some test fixtures do. isDebugNamingEnabled() stays false in that
        * case regardless.
        */
        inline const VkmEngineLaunchOptions& getLaunchOptions() const { return _launchOptions; }

        /*
        * @brief true if either enableValidationLayer or enableGpuCapture was requested at launch.
        * @details Resources and command queues push a native debug label only when this is true
        * and a debug name was supplied.
        */
        inline bool isDebugNamingEnabled() const { return _debugNamingEnabled; }

        /*
        * @brief The color format the swapchain is (or will be) created with.
        * @details Decided once at initialize() from the platform's non-HDR/HDR table and the
        * display's HDR capability (see selectSwapChainColorFormat). Known before any swapchain
        * object exists, so it is the single source of truth for both creating the swapchain and
        * resolving a pipeline's "swapchain" color format.
        */
        inline VkmFormat getSwapChainColorFormat() const { return _swapChainColorFormat; }

        /*
        * @brief What the graphics API reports about device memory right now -- the "actual"
        * counterpart to the per-resource totals in getRenderResourcePool()'s category usage.
        * @details Backends that cannot introspect their memory (WebGPU) keep this default, which
        * reports nothing rather than echoing the engine's own tracked numbers back as if measured.
        */
        virtual VkmGpuMemoryStats getGpuMemoryStats() const { return VkmGpuMemoryStats{}; }

        /*
        * @brief Whether this backend can place two textures over the same bytes -- i.e. whether
        * VkmResourceCreateInfo::Aliasable can be honored at all.
        * @details Vulkan (one VkDeviceMemory block bound at offsets) and Metal (one
        * MTLHeapTypePlacement heap) override this to true. WebGPU cannot: it exposes no
        * placement API, and its DontCare load op maps to WGPULoadOp_Load, so a first-use pass
        * would read whatever the other alias left behind. The default keeps the flag a portable
        * request that is warned about and cleared where it cannot be served.
        */
        virtual bool supportsResourceAliasing() const { return false; }

        /*
        * @brief Create/destroy one block of aliasing-heap memory. Called only by
        * VkmAliasedMemoryHeap, which owns the packing but no device memory.
        * @details `memoryTypeBits` is Vulkan's VkMemoryRequirements::memoryTypeBits, restricting
        * which memory type the block may come from; Metal passes ~0u since it has no equivalent.
        * Blocks are append-only and are destroyed only at driver teardown -- nothing reclaims one
        * while a placement still points into it.
        */
        virtual bool onCreateAliasBlock(uint32_t blockIndex, uint64_t sizeBytes, uint32_t memoryTypeBits)
        {
            (void)blockIndex; (void)sizeBytes; (void)memoryTypeBits;
            return false;
        }
        virtual void onDestroyAliasBlock(uint32_t blockIndex) { (void)blockIndex; }

        /*
        * @brief The packer that decides which Aliasable textures share bytes. Null on a backend
        * that cannot alias, so callers must check.
        */
        inline VkmAliasedMemoryHeap* getAliasedMemoryHeap() const { return _aliasedMemoryHeap.get(); }

#if defined(VKM_GPU_CAPTURE)
        /*
        * @brief Frame-boundary hooks called by VkmEngine::loopInner() on the render thread,
        * bracketing all of a frame's encoding, submission, and present. Only the Metal
        * backend overrides them (MTLCaptureScope begin/end for Xcode GPU capture).
        */
        virtual void onFrameBegin() {}
        virtual void onFrameEnd() {}

        /*
        * @brief Arms a GPU capture (.gputrace). Metal-only; the default is a no-op.
        * @details Requires enableGpuCapture at launch, since the capture scope only exists then.
        * @param startFrameDelay Frames to wait after the next onFrameBegin() before capturing.
        * @param frameCount Consecutive frames to capture.
        */
        virtual void requestGpuFrameCapture(uint32_t startFrameDelay = 0, uint32_t frameCount = 1)
        {
            (void)startFrameDelay; (void)frameCount;
        }
#endif // VKM_GPU_CAPTURE

        /*
        * @brief true if --enable-gpu-crash-dump was requested at launch.
        * @details Gates VkmGpuCrashHandler::recordSubmission()'s breadcrumb bookkeeping and, on
        * Vulkan, VK_EXT_device_fault enablement. Device-lost/error detection stays always-on.
        */
        inline bool isGpuCrashDumpEnabled() const { return _gpuCrashDumpEnabled; }

        /*
        * @brief get the GPU crash handler shared by every backend's VkmCommandQueueBase::submit()
        * override and device-lost/error detection path.
        */
        inline VkmGpuCrashHandler* getGpuCrashHandler() const { return _gpuCrashHandler.get(); }

        /*
        * @brief get the per-subgraph GPU profiler driven by VkmRenderGraph::execute() and read by
        * the debug overlay's "GPU" stat and VkmGpuProfilerInspector.
        * @details Never null after initialize(); reports isSupported() == false on a device without
        * timestamp queries.
        */
        inline VkmGpuProfiler* getGpuProfiler() const { return _gpuProfiler.get(); }

        /*
        * @brief Creates the GPU timestamp pool. Used only by VkmGpuProfiler.
        * @details A backend that cannot time GPU work leaves this returning false, and then none
        * of the other timestamp entry points is ever called. A backend that returns true must also
        * raise VkmDriverCapabilityFlags::TimestampQuery.
        * @param slotCount Flat number of timestamp slots; the profiler owns how they partition.
        * @return False if the backend has no timestamp support.
        */
        virtual bool initializeGpuTimestampPool(uint32_t slotCount) { (void)slotCount; return false; }
        virtual void destroyGpuTimestampPool() {}

        /*
        * @brief Nanoseconds per raw timestamp tick.
        * @return Vulkan's VkPhysicalDeviceLimits::timestampPeriod, or 1.0 on backends whose
        * timestamps are already nanoseconds.
        */
        virtual double getGpuTimestampPeriodNs() const { return 1.0; }

        /*
        * @brief Records the reset of a range of timestamp slots, ahead of the writes that fill them.
        * @details Must be recorded outside a render pass. Vulkan requires it before a slot can be
        * read back at all.
        * @param commandBuffer Command buffer to record into.
        * @param firstSlot First slot in the range.
        * @param count Number of slots to reset.
        */
        virtual void resetGpuTimestampSlots(VkmCommandBufferBase* commandBuffer, uint32_t firstSlot, uint32_t count)
        {
            (void)commandBuffer; (void)firstSlot; (void)count;
        }

        /*
        * @brief Reads raw timestamp ticks back from the pool.
        * @details Callers only ask once the submission that wrote them has completed, so a false
        * return means the read genuinely failed rather than "not yet".
        * @param firstSlot First slot to read.
        * @param count Number of slots to read.
        * @param outTicks Receives `count` raw ticks.
        * @return False if the read failed.
        */
        virtual bool resolveGpuTimestamps(uint32_t firstSlot, uint32_t count, uint64_t* outTicks)
        {
            (void)firstSlot; (void)count; (void)outTicks; return false;
        }

    protected:
        VkmCommandQueueBase* newCommandQueue(const VkmCommandQueueType queueType, const uint32_t commandQueueIndex, const char* name);

        /*
         * @brief Create a texture view referencing an existing (pooled) texture.
         * @details Friended to VkmTexture only: views must be created via VkmTexture::createView()
         * so ownership is tracked.
         */
        VkmTextureView* newTextureView(const VkmTextureViewInfo& info);

        /*
         * @brief Create a buffer view referencing an existing (pooled) buffer.
         * @details Friended to VkmBuffer only: views must be created via VkmBuffer::createView()
         * so ownership is tracked.
         */
        VkmBufferView* newBufferView(const VkmBufferViewInfo& info);

        friend class VkmTexture;
        friend class VkmBuffer;

    protected:
        virtual VkmInitResult initializeInner(const VkmEngineLaunchOptions* options) = 0;
        /*
        * @brief called at the end of initialize(), after the render resource pool and command
        * queues exist.
        * @details Backends whose bindless manager needs those -- Metal residency registration,
        * WebGPU queue-side uploads -- create it here.
        */
        virtual bool postInitializeInner() { return true; }
        virtual void destroyInner() = 0;
        virtual VkmTexture* newTextureInner() = 0;
        virtual VkmBuffer* newBufferInner() = 0;
        virtual VkmStagingBuffer* newStagingBufferInner() = 0;
        virtual VkmSampler* newSamplerInner() = 0;
        virtual VkmTextureView* newTextureViewInner() = 0;
        virtual VkmBufferView* newBufferViewInner() = 0;
        virtual VkmSwapChainBase* newSwapChainInner() = 0;
        virtual VkmResourceTableBase* newResourceTableInner() = 0;
        // Every backend implements this, including those without ray tracing: the WebGPU one logs
        // and returns null rather than splitting the base class behind an #ifdef.
        virtual VkmAccelerationStructure* newAccelerationStructureInner() = 0;
        // Non-pure with a null default: only backends whose capability flags advertise
        // TemporalUpscaling override this, so a backend without an upscaler needs no stub.
        virtual VkmUpscalerBase* newUpscalerInner() { return nullptr; }
        virtual VkmCommandQueueBase* newCommandQueueInner() = 0;
        virtual VkmPipelineStateBase* newPipelineStateInner() = 0;
        virtual VkmRenderResourcePool* newRenderResourcePoolInner() = 0;

        /*
        * @brief Choose the swapchain color format for this platform.
        * @details Called once from initialize(), after initializeInner().
        * @param enableHdr The launch request. A backend returns an HDR format only if it also
        * confirms the display supports it, and the non-HDR format otherwise.
        * @return The chosen color format.
        */
        virtual VkmFormat selectSwapChainColorFormat(bool enableHdr) const = 0;

    protected:
        std::array<std::vector<VkmCommandQueueBase*>, (uint8_t)VkmCommandQueueType::Count> _commandQueues;
        VkmDriverCapabilityFlags _driverCapabilityFlags;
        bool _hasUnifiedMemory = false;
        std::unique_ptr<VkmBindlessResourceManagerBase> _bindlessResourceManager;
        std::unique_ptr<VkmFrameConstantManagerBase> _frameConstantManager;

    private:
        std::unique_ptr<VkmRenderResourcePool> _renderResourcePool;
        // Created in initialize() only when supportsResourceAliasing(); stays null elsewhere so
        // the flag's sanitizer and compile()'s lifetime pass both short-circuit.
        std::unique_ptr<VkmAliasedMemoryHeap> _aliasedMemoryHeap;
        std::unique_ptr<VkmDeferredResourceReclaimer> _deferredReclaimer;
        std::unique_ptr<VkmGpuCrashHandler> _gpuCrashHandler;
        std::unique_ptr<VkmGpuProfiler> _gpuProfiler;
        VkmEngineLaunchOptions _launchOptions{};
        bool _debugNamingEnabled{false};
        VkmFormat _swapChainColorFormat{VkmFormat::Undefined};
        bool _gpuCrashDumpEnabled{false};
    };
}