// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_sampler.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#import <Metal/Metal.h>

namespace vkm
{
    namespace
    {
        MTLSamplerMinMagFilter toMTLFilter(VkmFilterMode filterMode)
        {
            return (filterMode == VkmFilterMode::Linear) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        }

        MTLSamplerMipFilter toMTLMipFilter(VkmMipmapMode mipmapMode)
        {
            return (mipmapMode == VkmMipmapMode::Linear) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        }

        MTLSamplerAddressMode toMTLAddressMode(VkmAddressMode addressMode)
        {
            switch (addressMode)
            {
                case VkmAddressMode::Repeat:         return MTLSamplerAddressModeRepeat;
                case VkmAddressMode::MirroredRepeat: return MTLSamplerAddressModeMirrorRepeat;
                case VkmAddressMode::ClampToEdge:    return MTLSamplerAddressModeClampToEdge;
                case VkmAddressMode::ClampToBorder:  return MTLSamplerAddressModeClampToBorderColor;
                default:                             return MTLSamplerAddressModeClampToEdge;
            }
        }

        MTLCompareFunction toMTLCompareFunction(VkmCompareOp compareOp)
        {
            switch (compareOp)
            {
                case VkmCompareOp::Never:          return MTLCompareFunctionNever;
                case VkmCompareOp::Less:           return MTLCompareFunctionLess;
                case VkmCompareOp::Equal:          return MTLCompareFunctionEqual;
                case VkmCompareOp::LessOrEqual:    return MTLCompareFunctionLessEqual;
                case VkmCompareOp::Greater:        return MTLCompareFunctionGreater;
                case VkmCompareOp::NotEqual:       return MTLCompareFunctionNotEqual;
                case VkmCompareOp::GreaterOrEqual: return MTLCompareFunctionGreaterEqual;
                case VkmCompareOp::Always:         return MTLCompareFunctionAlways;
                default:                           return MTLCompareFunctionNever;
            }
        }
    }

    VkmSamplerMetal::VkmSamplerMetal(VkmDriverBase* driver)
        : VkmSampler(driver)
    {
    }

    VkmSamplerMetal::~VkmSamplerMetal()
    {
        _mtlSampler = nil; // ARC releases
    }

    bool VkmSamplerMetal::initialize(VkmResourceHandle handle, const VkmSamplerInfo& info)
    {
        if (!initializeSamplerCommon(handle, info))
        {
            return false;
        }

        MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
        descriptor.minFilter = toMTLFilter(info._minFilter);
        descriptor.magFilter = toMTLFilter(info._magFilter);
        descriptor.mipFilter = toMTLMipFilter(info._mipmapMode);
        descriptor.sAddressMode = toMTLAddressMode(info._addressModeU);
        descriptor.tAddressMode = toMTLAddressMode(info._addressModeV);
        descriptor.rAddressMode = toMTLAddressMode(info._addressModeW);
        descriptor.maxAnisotropy = (info._maxAnisotropy > 1.0f) ? (NSUInteger)info._maxAnisotropy : 1;
        descriptor.compareFunction = info._compareEnable ? toMTLCompareFunction(info._compareOp) : MTLCompareFunctionNever;
        descriptor.lodMinClamp = info._minLod;
        descriptor.lodMaxClamp = info._maxLod;

        id<MTLDevice> device = static_cast<VkmDriverMetal*>(_driver)->getMTLDevice();
        _mtlSampler = [device newSamplerStateWithDescriptor:descriptor];
        if (_mtlSampler == nil)
        {
            VKM_DEBUG_ERROR("Failed to create MTLSamplerState");
            return false;
        }
        return true;
    }

    void VkmSamplerMetal::setDebugName(const char*)
    {
        // No-op: MTLSamplerState's label is read-only, fixed at creation from
        // MTLSamplerDescriptor.label -- there is no post-creation relabel API.
    }
} // namespace vkm
