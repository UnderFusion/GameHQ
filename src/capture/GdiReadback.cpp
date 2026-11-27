#include "capture/GdiReadback.h"

namespace GdiReadback
{
QImage readRect(HDC source, int x, int y, int w, int h, const Api& api)
{
    if (!source || w <= 0 || h <= 0)
        return {};

    // Allocated first: if the pixels cannot be held there is no GDI work to undo.
    QImage image(w, h, QImage::Format_RGB32);   // 0xffRRGGBB == BGRA in memory
    if (image.isNull())
        return {};

    HDC mem = api.createCompatibleDC(source);
    if (!mem)
        return {};
    HBITMAP bitmap = api.createCompatibleBitmap(source, w, h);
    if (!bitmap) {
        DeleteDC(mem);
        return {};
    }

    bool copied = false;
    if (HGDIOBJ previous = api.selectObject(mem, bitmap)) {
        copied = api.bitBlt(mem, 0, 0, w, h, source, x, y, SRCCOPY | CAPTUREBLT) != FALSE;
        // GetDIBits requires the bitmap not to be selected into a DC.
        api.selectObject(mem, previous);
    }

    bool read = false;
    if (copied) {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;   // top-down rows, matching QImage
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        // QImage rows are 32-bit aligned like DIB rows, so the scan lines land
        // directly in the image's own buffer: no second full-frame copy.
        read = api.getDIBits(mem, bitmap, 0, static_cast<UINT>(h), image.bits(), &info,
                             DIB_RGB_COLORS) == h;
    }

    DeleteObject(bitmap);
    DeleteDC(mem);
    return read ? image : QImage();
}
} // namespace GdiReadback
