// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>

#include <array>
#include <vector>
#include <mutex>

namespace vkm
{
    class VkmDriverBase;
    class VkmTexture;
    class VkmBuffer;
    class VkmStagingBuffer;
    class VkmSampler;
    class VkmTextureView;
    class VkmBufferView;

    class VkmRenderResourcePool
    {
    public:
        VkmRenderResourcePool(VkmDriverBase* driver);
        ~VkmRenderResourcePool();

        template <typename ResourceType>
        ResourceType* getResource(VkmResourceHandle handle);

        VkmResourceHandle allocateTexture(VkmTexture* texture, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateBuffer(VkmBuffer* buffer, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateStagingBuffer(VkmStagingBuffer* stagingBuffer, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateSampler(VkmSampler* sampler, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateTextureView(VkmTextureView* textureView, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        VkmResourceHandle allocateBufferView(VkmBufferView* bufferView, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        void releaseResource(VkmResourceHandle handle);\

    private:
        // Caller must already hold _mutex.
        VkmResourceHandle allocateResourceLocked(VkmResourceType type, VkmResourcePoolType poolType);

    public:
        class VkmDriverResourceSubPool
        {
            static constexpr const uint32_t POOL_GRANURARITY = 1024; // Maximum number of resources in a subpool
            friend class VkmRenderResourcePool;
        public:
            VkmDriverResourceSubPool();
            ~VkmDriverResourceSubPool();

        private:
            std::array<std::vector<std::unique_ptr<VkmDriverResourceBase>>, (uint8_t)VkmResourceType::Count> _resources;
            std::array<std::vector<uint32_t>, (uint8_t)VkmResourceType::Count> _generations;
            uint32_t _nextResourceId[(uint8_t)VkmResourceType::Count] = {0, };
        };

    private:
        VkmDriverBase* _driver;
        std::array<VkmDriverResourceSubPool, (uint8_t)VkmResourcePoolType::Count> _subPools;
        mutable std::mutex _mutex;
    };
}