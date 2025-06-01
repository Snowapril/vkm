// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

namespace vkm
{
    class VkmRenderGraph;
    class VkmDriverBase;
    class AppDelegate
    {
    public:
        AppDelegate() = default;
        virtual ~AppDelegate() = default;

    public:
        virtual void postDriverReady() = 0;
        virtual void preShutdown() = 0;
        virtual void update(const double deltaTime) = 0;
        virtual void render(VkmRenderGraph* renderGraph, VkmResourceHandle backBuffer) = 0;
        virtual const char* getAppName() const = 0;
    };
}