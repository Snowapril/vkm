// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

namespace vkm
{
    class AppDelegate
    {
    public:
        AppDelegate() = default;
        virtual ~AppDelegate() = default;

    public:
        virtual void onDriverInit() = 0;
        virtual void onShutdown() = 0;
        virtual void onUpdate(const double deltaTime) = 0;
        virtual void onRender(VkmResourceHandle currentBackBuffer) = 0;
        virtual const char* getAppName() const = 0;
    };
}