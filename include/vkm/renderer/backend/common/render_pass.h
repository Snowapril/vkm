// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

namespace vkm
{
    constexpr const uint32_t MAX_COLOR_ATTACHMENTS = 8; // Maximum number of color attachments in a render pass

    enum class VkmLoadAction : uint8_t
    {
        Load, // Load the previous contents of the attachment
        Clear, // Clear the attachment to a specified value
        DontCare // Do not care about the previous contents of the attachment
    };

    enum class VkmStoreAction : uint8_t
    {
        Store, // Store the contents of the attachment after rendering
        DontCare // Do not store the contents of the attachment
    };

    struct VkmColorAttachmentDescriptor
    {
        uint32_t _attachmentId; // Unique identifier for the color attachment
        VkmLoadAction _loadAction; // Load action for the color attachment
        VkmStoreAction _storeAction; // Store action for the color attachment
        float _clearColors[4]; // Clear color value (RGBA)

        inline bool operator==(const VkmColorAttachmentDescriptor& other) const noexcept
        {
            return _attachmentId == other._attachmentId &&
                   _loadAction == other._loadAction &&
                   _storeAction == other._storeAction &&
                   _clearColors[0] == other._clearColors[0] &&
                   _clearColors[1] == other._clearColors[1] &&
                   _clearColors[2] == other._clearColors[2] &&
                   _clearColors[3] == other._clearColors[3];
        }
        inline bool operator!=(const VkmColorAttachmentDescriptor& other) const noexcept
        {
            return !(*this == other);
        }
    };

    struct VkmDepthStencilAttachmentDescriptor
    {
        uint32_t _attachmentId; // Unique identifier for the depth/stencil attachment
        VkmLoadAction _loadAction; // Load action for the depth/stencil attachment
        VkmStoreAction _storeAction; // Store action for the depth/stencil attachment
        float _clearDepth; // Clear depth value
        uint32_t _clearStencil; // Clear stencil value

        inline bool operator==(const VkmDepthStencilAttachmentDescriptor& other) const noexcept
        {
            return _attachmentId == other._attachmentId &&
                   _loadAction == other._loadAction &&
                   _storeAction == other._storeAction &&
                   _clearDepth == other._clearDepth &&
                   _clearStencil == other._clearStencil;
        }
        inline bool operator!=(const VkmDepthStencilAttachmentDescriptor& other) const noexcept
        {
            return !(*this == other);
        }
    };

    // Render pass descriptor
    struct VkmRenderPassDescriptor
    {
        uint32_t _colorAttachmentCount; // Number of color attachments
        std::array<VkmColorAttachmentDescriptor, MAX_COLOR_ATTACHMENTS> _colorAttachments; // Array of color attachments
        std::optional<VkmDepthStencilAttachmentDescriptor> _depthStencilAttachment; // Depth/stencil attachment descriptor

        inline bool operator==(const VkmRenderPassDescriptor& other) const noexcept
        {
            return _colorAttachmentCount == other._colorAttachmentCount &&
                   _colorAttachments == other._colorAttachments &&
                   _depthStencilAttachment == other._depthStencilAttachment;
        }
        inline bool operator!=(const VkmRenderPassDescriptor& other) const noexcept
        {
            return !(*this == other);
        }
    };

    // Frame buffer descriptor
    // Contains information about the framebuffer used in a render pass
    struct VkmFrameBufferDescriptor
    {
        VkmRenderPassDescriptor _renderPass; // Render pass descriptor
        uint32_t _width; // Width of the framebuffer
        uint32_t _height; // Height of the framebuffer
        
        std::array<VkmResourceHandle, MAX_COLOR_ATTACHMENTS> _colorAttachments; // Handles to color attachments
        std::optional<VkmResourceHandle> _depthStencilAttachment; // Handle to the depth/stencil attachment

        inline bool operator==(const VkmFrameBufferDescriptor& other) const noexcept
        {
            return _renderPass == other._renderPass &&
                   _width == other._width &&
                   _height == other._height &&
                   _colorAttachments == other._colorAttachments &&
                   _depthStencilAttachment == other._depthStencilAttachment;
        }
        inline bool operator!=(const VkmFrameBufferDescriptor& other) const noexcept
        {
            return !(*this == other);
        }
    };
}

template <>
struct std::hash<vkm::VkmColorAttachmentDescriptor>
{
    std::size_t operator()(const vkm::VkmColorAttachmentDescriptor& descriptor) const noexcept
    {
        return std::hash<uint32_t>()(descriptor._attachmentId) ^
               std::hash<uint8_t>()(static_cast<uint8_t>(descriptor._loadAction)) ^
               std::hash<uint8_t>()(static_cast<uint8_t>(descriptor._storeAction)) ^
               std::hash<uint32_t>()(descriptor._clearColors[0]) ^
               std::hash<uint32_t>()(descriptor._clearColors[1]) ^
               std::hash<uint32_t>()(descriptor._clearColors[2]) ^
               std::hash<uint32_t>()(descriptor._clearColors[3]);
    }
};

template <>
struct std::hash<vkm::VkmDepthStencilAttachmentDescriptor>
{
    std::size_t operator()(const vkm::VkmDepthStencilAttachmentDescriptor& descriptor) const noexcept
    {
        return std::hash<uint32_t>()(descriptor._attachmentId) ^
               std::hash<uint8_t>()(static_cast<uint8_t>(descriptor._loadAction)) ^
               std::hash<uint8_t>()(static_cast<uint8_t>(descriptor._storeAction)) ^
               std::hash<uint32_t>()(descriptor._clearDepth) ^
               std::hash<uint32_t>()(descriptor._clearStencil);
    }
};

template <>
struct std::hash<vkm::VkmRenderPassDescriptor>
{
    std::size_t operator()(const vkm::VkmRenderPassDescriptor& descriptor) const noexcept
    {
        std::size_t hash = 0;
        for (const auto& attachment : descriptor._colorAttachments)
        {
            hash ^= std::hash<vkm::VkmColorAttachmentDescriptor>()(attachment);
        }
        if (descriptor._depthStencilAttachment.has_value())
        {
            hash ^= std::hash<vkm::VkmDepthStencilAttachmentDescriptor>()(*descriptor._depthStencilAttachment);
        }
        return hash ^ std::hash<uint32_t>()(descriptor._colorAttachmentCount);
    }
};

template <>
struct std::hash<vkm::VkmFrameBufferDescriptor>
{
    std::size_t operator()(const vkm::VkmFrameBufferDescriptor& descriptor) const noexcept
    {
        std::size_t hash = std::hash<vkm::VkmRenderPassDescriptor>()(descriptor._renderPass) ^
                           std::hash<uint32_t>()(descriptor._width) ^
                           std::hash<uint32_t>()(descriptor._height);
        for (const auto& colorAttachment : descriptor._colorAttachments)
        {
            hash ^= std::hash<vkm::VkmResourceHandle>()(colorAttachment);
        }
        if (descriptor._depthStencilAttachment.has_value())
        {
            hash ^= std::hash<vkm::VkmResourceHandle>()(*descriptor._depthStencilAttachment);
        }
        return hash;
    }
};