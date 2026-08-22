// Copyright (c) 2026 Snowapril

#include <vkm/renderer/gi_composite.h>

namespace vkm
{
    const char* vkmGiDebugViewName(VkmGiDebugView view)
    {
        switch (view)
        {
            case VkmGiDebugView::Composite:       return "Composite";
            case VkmGiDebugView::Direct:          return "Direct only";
            case VkmGiDebugView::Indirect:        return "Indirect only";
            case VkmGiDebugView::Albedo:          return "Albedo";
            case VkmGiDebugView::Normal:          return "Shading normal";
            case VkmGiDebugView::GeometricNormal: return "Geometric normal";
            case VkmGiDebugView::Roughness:       return "Roughness";
            case VkmGiDebugView::Metallic:        return "Metallic";
            case VkmGiDebugView::Motion:          return "Motion vectors";
            case VkmGiDebugView::CameraDistance:  return "Camera distance";
            case VkmGiDebugView::Emissive:        return "Emissive";
            case VkmGiDebugView::Count:           break;
        }
        return "Unknown";
    }
} // namespace vkm
