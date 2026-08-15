// Copyright (c) 2025 Snowapril

#pragma once

#if defined(VKM_ENABLE_FSR)

#include <vkm/renderer/backend/common/upscaler.h>

#include <ffx_api/ffx_api.h>

namespace vkm
{
    /*
    * @brief FSR 3.1 temporal upscaler: wraps a FidelityFX ffx-api upscale context behind
    * VkmUpscalerBase.
    * @details Loads AMD's signed amd_fidelityfx_vk.dll at initialize() and records the upscale
    * into the subgraph's VkCommandBuffer through ffxDispatch. Resource states handed to the
    * runtime mirror the layouts the render graph's barrier plan placed: inputs
    * SHADER_READ_ONLY_OPTIMAL (COMPUTE_READ), output GENERAL (UNORDERED_ACCESS); the runtime
    * restores them after the dispatch.
    */
    class VkmUpscalerVulkan final : public VkmUpscalerBase
    {
    public:
        VkmUpscalerVulkan() = default;
        ~VkmUpscalerVulkan() override = default;

    protected:
        virtual bool initializeInner() override final;
        virtual void destroyInner() override final;
        virtual void encodeInner(VkmCommandBufferBase* commandBuffer,
                                 const VkmUpscalerDispatchDesc& dispatchDesc) override final;

    private:
        void* _ffxModule = nullptr; // HMODULE, held as void* to keep windows.h out of this header
        ffxContext _context = nullptr;
        PfnFfxCreateContext _createContext = nullptr;
        PfnFfxDestroyContext _destroyContext = nullptr;
        PfnFfxDispatch _dispatch = nullptr;
    };
} // namespace vkm

#endif // VKM_ENABLE_FSR
