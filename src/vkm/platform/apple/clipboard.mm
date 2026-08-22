// Copyright (c) 2025 Snowapril

#include <vkm/platform/common/clipboard.h>
#include <vkm/base/common.h>

#import <AppKit/AppKit.h>

#include <cstring>

namespace vkm
{
    bool vkmSetClipboardImage(uint32_t width, uint32_t height, const uint8_t* rgba8)
    {
        @autoreleasepool
        {
            if (rgba8 == nullptr || width == 0 || height == 0)
            {
                VKM_DEBUG_ERROR("vkmSetClipboardImage: invalid arguments");
                return false;
            }

            // This target builds without ARC, so the representation is autoreleased into the
            // pool above rather than released on every exit path.
            NSBitmapImageRep* rep = [[[NSBitmapImageRep alloc]
                initWithBitmapDataPlanes:NULL
                              pixelsWide:width
                              pixelsHigh:height
                           bitsPerSample:8
                         samplesPerPixel:4
                                hasAlpha:YES
                                isPlanar:NO
                          colorSpaceName:NSDeviceRGBColorSpace
                             bytesPerRow:width * 4
                            bitsPerPixel:32] autorelease];
            if (rep == nil)
            {
                VKM_DEBUG_ERROR("vkmSetClipboardImage: failed to create NSBitmapImageRep");
                return false;
            }

            uint8_t* dst = [rep bitmapData];
            if (dst == nullptr)
            {
                VKM_DEBUG_ERROR("vkmSetClipboardImage: NSBitmapImageRep has no backing store");
                return false;
            }
            std::memcpy(dst, rgba8, static_cast<size_t>(width) * height * 4);

            NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            if (png == nil)
            {
                VKM_DEBUG_ERROR("vkmSetClipboardImage: failed to encode PNG representation");
                return false;
            }

            NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
            [pasteboard clearContents];
            BOOL ok = [pasteboard setData:png forType:NSPasteboardTypePNG];
            if (!ok)
            {
                VKM_DEBUG_ERROR("vkmSetClipboardImage: NSPasteboard setData failed");
                return false;
            }

            return true;
        }
    }
} // namespace vkm
