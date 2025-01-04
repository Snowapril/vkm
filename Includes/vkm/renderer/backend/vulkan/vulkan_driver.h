// Copyright (c) 2024 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>

namespace vkm
{
    /*
    * @brief renderer backend driver base class
    * @details 
    */
    class VkmDriverVulkan : public VkmDriverBase
    {
    public:
        VkmDriverVulkan();
        ~VkmDriverVulkan();

    protected:
        virtual bool initializeInner() override final;
        virtual void destroyInner() override final;

    private:
    };
}