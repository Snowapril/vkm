// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/render_graph_barrier.h>
#include <volk.h>

namespace vkm
{
    /*
    * Maps the engine's backend-neutral access vocabulary onto Vulkan's. Kept out of
    * vulkan_command_buffer.cpp so the tables can be read (and tested) on their own, and because
    * this is the one place a wrong entry turns into a synchronisation bug that reproduces once
    * every few thousand frames.
    */

    /*
    * @brief Which pipeline stages an access runs in.
    *
    * @details `scope` decides the shader stages, because the access alone cannot: a sampled read
    * is a fragment read in a graphics subgraph and a compute read in a compute one. Accesses that
    * carry their own stage regardless -- attachments, indirect fetch, transfers, acceleration
    * structure builds -- ignore it.
    *
    * VkmResourceAccess::None means "produced outside this render graph": a host upload, a previous
    * frame, or a swapchain image just acquired. Nothing here knows which, so it maps to
    * ALL_COMMANDS, matching what the engine's barriers did unconditionally before this existed.
    */
    VkPipelineStageFlags2 vkmToVkStageMask(VkmResourceAccess access, VkmPipelineScope scope);

    // The access mask that goes with it. Read-only sources in an execution-only barrier drop this
    // entirely -- a read publishes nothing that needs flushing -- which the caller handles.
    VkAccessFlags2 vkmToVkAccessMask(VkmResourceAccess access);

    /*
    * @brief The layout a texture must be in for `access`, or VK_IMAGE_LAYOUT_UNDEFINED for an
    * access that names no layout (a buffer access, or None).
    *
    * `format` decides only whether a depth/stencil read wants the read-only depth layout.
    */
    VkImageLayout vkmToVkImageLayout(VkmResourceAccess access, VkmFormat format);

    // The aspect a barrier on this format has to name. A sampled depth texture -- a shadow map, or
    // the G-buffer depth a GI pass reads -- needs the depth/stencil aspects, not the colour one.
    VkImageAspectFlags vkmToVkAspectMask(VkmFormat format);
}
