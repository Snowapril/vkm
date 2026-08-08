// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <optional>

namespace vkm
{
    constexpr const uint32_t MAX_COLOR_ATTACHMENTS = 8;

    enum class VkmLoadAction : uint8_t
    {
        Load,
        Clear,
        DontCare
    };

    enum class VkmStoreAction : uint8_t
    {
        Store,
        DontCare
    };

    struct VkmColorAttachmentDescriptor
    {
        uint32_t _attachmentId;
        VkmLoadAction _loadAction;
        VkmStoreAction _storeAction;
        float _clearColors[4];  // RGBA

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
        uint32_t _attachmentId;
        VkmLoadAction _loadAction;
        VkmStoreAction _storeAction;
        float _clearDepth;
        uint32_t _clearStencil;

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

    struct VkmRenderPassDescriptor
    {
        uint32_t _colorAttachmentCount;
        std::array<VkmColorAttachmentDescriptor, MAX_COLOR_ATTACHMENTS> _colorAttachments;
        std::optional<VkmDepthStencilAttachmentDescriptor> _depthStencilAttachment;

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

    struct VkmFrameBufferDescriptor
    {
        VkmRenderPassDescriptor _renderPass;
        uint32_t _width;
        uint32_t _height;

        std::array<VkmResourceHandle, MAX_COLOR_ATTACHMENTS> _colorAttachments;
        std::optional<VkmResourceHandle> _depthStencilAttachment;

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
               std::hash<uint32_t>()(static_cast<uint32_t>(descriptor._clearColors[0])) ^
               std::hash<uint32_t>()(static_cast<uint32_t>(descriptor._clearColors[1])) ^
               std::hash<uint32_t>()(static_cast<uint32_t>(descriptor._clearColors[2])) ^
               std::hash<uint32_t>()(static_cast<uint32_t>(descriptor._clearColors[3]));
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
               std::hash<uint32_t>()(static_cast<uint32_t>(descriptor._clearDepth)) ^
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
