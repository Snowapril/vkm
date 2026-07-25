// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/platform/common/window.h>
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
    class VkmSwapChainBase;
    class VkmCommandQueueBase;
    class VkmCommandDispatcher;
    class VkmRenderResourcePool;
    class VkmPipelineStateBase;
    class VkmDeferredResourceReclaimer;
    class VkmGpuCrashHandler;
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
        // Backend implements copyBufferToTexture, and therefore uploadToTexture and the
        // bindless texture array. WebGPU does not: it has no unsized texture arrays to bind
        // into (its bindless layer is mega-buffer emulation), so nothing there can sample a
        // texture even once the pixels are uploaded.
        TextureUpload           = 0x00000004,
        // Backend can write a texture's memory from the CPU, so uploadToTexture can skip the
        // staging buffer and the queue submit entirely. Requires both the mechanism (Metal:
        // MTLStorageModeShared; Vulkan: VK_EXT_host_image_copy) and a reason to use it
        // (unified memory) -- see VkmTextureUploadMode. A texture still only takes that path
        // when its own memory allows it: VkmTexture::isHostWritable is the per-texture answer,
        // this flag is the per-device precondition.
        TextureHostCopy         = 0x00000008,
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
        * @brief desrtroy all resources and clean up
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
         * uncompressed color texture, via a transient readback staging buffer and a one-off
         * command buffer on the Graphics queue. Blocks until the copy completes -- intended
         * for tests and debugging, not per-frame use. Returns empty pixels on failure.
         */
        VkmTextureReadbackResult readbackTexture(VkmResourceHandle textureHandle, uint32_t arrayLayer = 0);

        /*
         * @brief Synchronously upload `size` bytes from `data` into `dstBuffer` at
         * `dstOffset`, via a transient staging buffer and a one-off command buffer
         * submitted to the Graphics queue (this engine has no dedicated Transfer queue).
         * Blocks until the GPU copy completes -- intended for setup-time uploads (e.g.
         * postDriverReady), not per-frame streaming.
         */
        bool uploadToBuffer(VkmResourceHandle dstBuffer, const void* data, uint64_t size, uint64_t dstOffset = 0);

        /*
         * @brief Synchronously upload `size` bytes of tightly-packed pixels into one mip
         * level of one array layer of `dstTexture` -- the texture counterpart of
         * uploadToBuffer, with the same transient-staging, one-off-command-buffer, blocking
         * shape and the same setup-time-only intent. A cubemap is six calls, one per face,
         * with arrayLayer running over VkmTextureInfo's +X, -X, +Y, -Y, +Z, -Z order.
         * The texture is sampleable once this returns.
         *
         * `mode` selects how the pixels get there; the default picks the direct CPU write
         * when the destination texture's memory allows it and the staging copy otherwise
         * (see VkmTextureUploadMode). The direct write does no queue work at all, so it does
         * not block -- but it is only available where the memory type permits, which is why
         * this stays a single entry point rather than two.
         */
        bool uploadToTexture(VkmResourceHandle dstTexture, const void* data, uint64_t size,
                             uint32_t mipLevel = 0, uint32_t arrayLayer = 0,
                             VkmTextureUploadMode mode = VkmTextureUploadMode::Auto);

        /*
         * @brief Create sampler with the given sampler info
         */
        VkmSampler* newSampler(const VkmSamplerInfo& info);

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
        * @brief get driver capability flags
        */
        inline VkmDriverCapabilityFlags getDriverCapabilityFlags() const { return _driverCapabilityFlags; }

        /*
        * @brief Whether the CPU and GPU share one memory pool, so a texture placed in
        * CPU-writable memory costs the GPU nothing to sample.
        * @details Set by each backend during initializeInner (Metal: hasUnifiedMemory;
        * Vulkan: a DEVICE_LOCAL|HOST_VISIBLE memory type exists). This is what gates the
        * host-copy texture upload path -- see VkmTextureUploadMode and
        * VkmTexture::isHostWritable. False on backends that never checked.
        */
        inline bool hasUnifiedMemory() const { return _hasUnifiedMemory; }

        // @brief get command dispatcher for the driver
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
        * common/bindless_resource_manager.h). Created by each backend driver during
        * initialization (Vulkan in initializeInner since pipeline layouts need the set
        * layout; Metal/WebGPU in postInitializeInner since they need the resource pool
        * and queues).
        */
        inline VkmBindlessResourceManagerBase* getBindlessResourceManager() const { return _bindlessResourceManager.get(); }

        /*
        * @brief get the engine-global per-frame constant manager (set 1 convention -- see
        * common/frame_constants.h). Created by each backend driver during initialization,
        * next to the bindless manager. VkmEngine::render() writes one frame slot through it
        * per frame; every pipeline bind publishes that slot.
        */
        inline VkmFrameConstantManagerBase* getFrameConstantManager() const { return _frameConstantManager.get(); }

        /*
        * @brief get deferred resource reclaimer; VkmRenderGraph drives its per-frame
        * pollOnce() fallback on WASM through this accessor.
        */
        inline VkmDeferredResourceReclaimer* getDeferredReclaimer() const { return _deferredReclaimer.get(); }

        /*
        * @brief get the launch options this driver was initialized with. If initialize()
        * was called with a null options pointer (some test fixtures do this), this returns
        * DEFAULT_ENGINE_LAUNCH_OPTIONS -- but note isDebugNamingEnabled() is still false in
        * that case regardless (see initialize()'s null-guard), since a null-options caller
        * has explicitly opted out of the whole launch-configuration story.
        */
        inline const VkmEngineLaunchOptions& getLaunchOptions() const { return _launchOptions; }

        /*
        * @brief true if either enableValidationLayer or enableGpuCapture was requested at
        * launch. Resources (see newTexture/newBuffer/etc. in driver.cpp) and command queues
        * (VkmCommandQueueBase::initialize()) only push a native debug label when this is
        * true AND a debug name was actually supplied.
        */
        inline bool isDebugNamingEnabled() const { return _debugNamingEnabled; }

        /*
        * @brief The color format the swapchain is (or will be) created with, decided once at
        * initialize() from the platform's non-HDR/HDR table and the display's HDR capability
        * (see selectSwapChainColorFormat). Known before any swapchain object exists, so it is
        * the single source of truth used both to create the swapchain and to resolve a
        * pipeline's "swapchain" color format (newPipelineState).
        */
        inline VkmFormat getSwapChainColorFormat() const { return _swapChainColorFormat; }

        /*
        * @brief What the graphics API reports about device memory right now -- the "actual"
        * counterpart to the per-resource totals in getRenderResourcePool()'s category usage.
        * @details Backends that cannot introspect their memory (WebGPU) keep this default,
        * which reports nothing rather than echoing the engine's own tracked numbers back as
        * if they were measured.
        */
        virtual VkmGpuMemoryStats getGpuMemoryStats() const { return VkmGpuMemoryStats{}; }

#if defined(VKM_GPU_CAPTURE)
        /*
        * @brief Frame-boundary hooks called by VkmEngine::loopInner() on the render thread,
        * bracketing all of a frame's encoding, submission, and present. Only the Metal
        * backend overrides them (MTLCaptureScope begin/end for Xcode GPU capture).
        */
        virtual void onFrameBegin() {}
        virtual void onFrameEnd() {}

        /*
        * @brief Arm a GPU capture (.gputrace) starting `startFrameDelay` frames after the
        * next onFrameBegin() and spanning `frameCount` consecutive frames. Metal-only;
        * default is a no-op. Requires enableGpuCapture at launch (the capture scope only
        * exists then).
        */
        virtual void requestGpuFrameCapture(uint32_t startFrameDelay = 0, uint32_t frameCount = 1)
        {
            (void)startFrameDelay; (void)frameCount;
        }
#endif // VKM_GPU_CAPTURE

        /*
        * @brief true if --enable-gpu-crash-dump was requested at launch. Gates
        * VkmGpuCrashHandler::recordSubmission()'s breadcrumb bookkeeping and (on Vulkan)
        * VK_EXT_device_fault extension enablement. Device-lost/error detection itself stays
        * always-on regardless of this flag -- see gpu_crash_handler.h.
        */
        inline bool isGpuCrashDumpEnabled() const { return _gpuCrashDumpEnabled; }

        /*
        * @brief get the GPU crash handler shared by every backend's VkmCommandQueueBase::submit()
        * override and device-lost/error detection path.
        */
        inline VkmGpuCrashHandler* getGpuCrashHandler() const { return _gpuCrashHandler.get(); }

    protected:
        VkmCommandQueueBase* newCommandQueue(const VkmCommandQueueType queueType, const uint32_t commandQueueIndex, const char* name);

        /*
         * @brief Create a texture view referencing an existing (pooled) texture. Protected
         * and friended to VkmTexture only -- views must be created via
         * VkmTexture::createView() so ownership is tracked; nothing else may call this.
         */
        VkmTextureView* newTextureView(const VkmTextureViewInfo& info);

        /*
         * @brief Create a buffer view referencing an existing (pooled) buffer. Protected
         * and friended to VkmBuffer only -- views must be created via
         * VkmBuffer::createView() so ownership is tracked; nothing else may call this.
         */
        VkmBufferView* newBufferView(const VkmBufferViewInfo& info);

        friend class VkmTexture;
        friend class VkmBuffer;

    protected:
        virtual VkmInitResult initializeInner(const VkmEngineLaunchOptions* options) = 0;
        /*
        * @brief called at the end of initialize(), after the render resource pool and
        * command queues exist. Backends whose bindless manager needs those (Metal residency
        * registration, WebGPU queue-side uploads) create it here.
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
        virtual VkmCommandQueueBase* newCommandQueueInner() = 0;
        virtual VkmPipelineStateBase* newPipelineStateInner() = 0;
        virtual VkmRenderResourcePool* newRenderResourcePoolInner() = 0;

        /*
        * @brief Choose the swapchain color format for this platform. `enableHdr` is the launch
        * request; a backend returns an HDR format only if it also confirms the display supports
        * it, otherwise the non-HDR format. Called once from initialize() after initializeInner().
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
        std::unique_ptr<VkmDeferredResourceReclaimer> _deferredReclaimer;
        std::unique_ptr<VkmGpuCrashHandler> _gpuCrashHandler;
        VkmEngineLaunchOptions _launchOptions{};
        bool _debugNamingEnabled{false};
        VkmFormat _swapChainColorFormat{VkmFormat::Undefined};
        bool _gpuCrashDumpEnabled{false};
    };
}