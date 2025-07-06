// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>

#include <array>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmTexture;

    class VkmRenderResourcePool
    {
    public:
        VkmRenderResourcePool(VkmDriverBase* driver);
        ~VkmRenderResourcePool();

        template <typename ResourceType>
        ResourceType* getResource(VkmResourceHandle handle);

        VkmResourceHandle allocateTexture(VkmTexture* texture, VkmResourcePoolType poolType = VkmResourcePoolType::Default);
        void releaseResource(VkmResourceHandle handle);\

    private:
        VkmResourceHandle allocateResource(VkmResourceType type, VkmResourcePoolType poolType);

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
            uint32_t _nextResourceId[(uint8_t)VkmResourceType::Count] = {0, };
        };

    private:
        VkmDriverBase* _driver;
        std::array<VkmDriverResourceSubPool, (uint8_t)VkmResourcePoolType::Count> _subPools;
    };
}