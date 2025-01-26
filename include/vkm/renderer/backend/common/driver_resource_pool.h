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
    
    class VkmDriverResourcePool
    {
    public:
        VkmDriverResourcePool(VkmDriverBase* driver);
        ~VkmDriverResourcePool();

        template <typename ResourceType>
        ResourceType* getResource(VkmResourceHandle handle);

    public:
        class VkmDriverResourceSubPool
        {
            friend class VkmDriverResourcePool;
        public:
            VkmDriverResourceSubPool();
            ~VkmDriverResourceSubPool();

        private:
            std::array<std::vector<std::unique_ptr<IVkmDriverResource>>, (uint8_t)VkmResourceType::Count> _resources;
        };

    private:
        VkmDriverBase* _driver;
        std::array<VkmDriverResourceSubPool, (uint8_t)VkmResourcePoolType::Count> _subPools;
    };
}