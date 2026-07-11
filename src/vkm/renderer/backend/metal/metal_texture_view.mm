// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_texture_view.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_util.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

#import <Metal/Metal.h>

namespace vkm
{
    VkmTextureViewMetal::VkmTextureViewMetal(VkmDriverBase* driver)
        : VkmTextureView(driver)
    {
    }

    VkmTextureViewMetal::~VkmTextureViewMetal()
    {
        _mtlTextureView = nil; // ARC releases
    }

    bool VkmTextureViewMetal::initialize(VkmResourceHandle handle, const VkmTextureViewInfo& info)
    {
        if (!initializeTextureViewCommon(handle, info))
        {
            return false;
        }

        if (info._texture.type != VkmResourceType::Texture)
        {
            VKM_DEBUG_ERROR("VkmTextureViewInfo::_texture must reference a Texture handle");
            return false;
        }

        VkmTextureMetal* parentTexture = static_cast<VkmTextureMetal*>(resolveParent());
        if (parentTexture == nullptr)
        {
            return false;
        }

        const VkmTextureInfo& parentInfo = parentTexture->getTextureInfo();
        const VkmFormat resolvedFormat = (info._format != VkmFormat::Undefined) ? info._format : parentInfo._format;
        const uint32_t numMipLevels = (info._numMipLevels != UINT32_MAX)
            ? info._numMipLevels
            : (parentInfo._numMipLevels - info._baseMipLevel);
        const uint32_t numArrayLayers = (info._numArrayLayers != UINT32_MAX)
            ? info._numArrayLayers
            : (parentInfo._numArrayLayers - info._baseArrayLayer);

        const MTLTextureType textureType = (numArrayLayers > 1) ? MTLTextureType2DArray : MTLTextureType2D;

        id<MTLTexture> parentMtlTexture = parentTexture->getInternalHandle();
        _mtlTextureView = [parentMtlTexture newTextureViewWithPixelFormat:getMTLPixelFormat(resolvedFormat)
                                                                textureType:textureType
                                                                     levels:NSMakeRange(info._baseMipLevel, numMipLevels)
                                                                     slices:NSMakeRange(info._baseArrayLayer, numArrayLayers)];
        if (_mtlTextureView == nil)
        {
            VKM_DEBUG_ERROR("Failed to create MTLTexture view");
            return false;
        }
        return true;
    }

    void VkmTextureViewMetal::setDebugName(const char* name)
    {
        [_mtlTextureView setLabel:[NSString stringWithUTF8String:name]];
    }
} // namespace vkm
