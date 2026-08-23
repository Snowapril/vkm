// Copyright (c) 2025 Snowapril

#include <vkm/platform/common/clipboard.h>
#include <vkm/base/common.h>

#include <windows.h>

#include <cstring>

namespace vkm
{
    bool vkmSetClipboardImage(uint32_t width, uint32_t height, const uint8_t* rgba8)
    {
        if (rgba8 == nullptr || width == 0 || height == 0)
        {
            VKM_DEBUG_ERROR("vkmSetClipboardImage: invalid arguments");
            return false;
        }

        const size_t pixelBytes = static_cast<size_t>(width) * height * 4;
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixelBytes);
        if (handle == nullptr)
        {
            VKM_DEBUG_ERROR("vkmSetClipboardImage: GlobalAlloc failed");
            return false;
        }

        void* memory = GlobalLock(handle);
        if (memory == nullptr)
        {
            GlobalFree(handle);
            VKM_DEBUG_ERROR("vkmSetClipboardImage: GlobalLock failed");
            return false;
        }

        BITMAPINFOHEADER* header = static_cast<BITMAPINFOHEADER*>(memory);
        std::memset(header, 0, sizeof(BITMAPINFOHEADER));
        header->biSize = sizeof(BITMAPINFOHEADER);
        header->biWidth = static_cast<LONG>(width);
        header->biHeight = static_cast<LONG>(height);
        header->biPlanes = 1;
        header->biBitCount = 32;
        header->biCompression = BI_RGB;
        header->biSizeImage = static_cast<DWORD>(pixelBytes);

        // CF_DIB rows are stored bottom-up, and each pixel is ordered BGRA rather than RGBA.
        uint8_t* dst = static_cast<uint8_t*>(memory) + sizeof(BITMAPINFOHEADER);
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* srcRow = rgba8 + static_cast<size_t>(height - 1 - y) * width * 4;
            uint8_t* dstRow = dst + static_cast<size_t>(y) * width * 4;
            for (uint32_t x = 0; x < width; ++x)
            {
                dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
                dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
            }
        }

        GlobalUnlock(handle);

        if (!OpenClipboard(nullptr))
        {
            GlobalFree(handle);
            VKM_DEBUG_ERROR("vkmSetClipboardImage: OpenClipboard failed");
            return false;
        }

        if (!EmptyClipboard())
        {
            CloseClipboard();
            GlobalFree(handle);
            VKM_DEBUG_ERROR("vkmSetClipboardImage: EmptyClipboard failed");
            return false;
        }

        if (SetClipboardData(CF_DIB, handle) == nullptr)
        {
            CloseClipboard();
            GlobalFree(handle);
            VKM_DEBUG_ERROR("vkmSetClipboardImage: SetClipboardData failed");
            return false;
        }

        // The clipboard now owns handle; it must not be freed here.
        if (!CloseClipboard())
        {
            VKM_DEBUG_ERROR("vkmSetClipboardImage: CloseClipboard failed");
            return false;
        }

        return true;
    }
} // namespace vkm
