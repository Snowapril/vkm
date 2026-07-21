// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

namespace vkm
{
    class VkmRenderGraph;
    class VkmDriverBase;
    class VkmEngine;
    class AppDelegate
    {
    public:
        AppDelegate() = default;
        virtual ~AppDelegate() = default;

    public:
        virtual void postDriverReady(VkmEngine* engine) = 0;
        virtual void preShutdown() = 0;
        virtual void update(const double deltaTime) = 0;
        // Called once per non-ImGui window per frame. windowIndex identifies the target window
        // (see VkmEngine::addSwapChain), letting an app render different content per window.
        virtual void render(uint32_t windowIndex, VkmRenderGraph* renderGraph, VkmResourceHandle backBuffer) = 0;
        virtual const char* getAppName() const = 0;
    };
}