#pragma once

#include <QImage>

#include <windows.h>

// Copies a rectangle of a device context into a QImage through a compatible
// bitmap, following the documented GDI contract: the bitmap is deselected
// from the memory DC before GetDIBits reads it. The returned image owns its
// pixels; nothing in it refers to GDI memory released before returning.
namespace GdiReadback
{
// The GDI calls the readback makes, replaceable so tests can fail each one.
struct Api
{
    decltype(&::CreateCompatibleDC) createCompatibleDC = &::CreateCompatibleDC;
    decltype(&::CreateCompatibleBitmap) createCompatibleBitmap = &::CreateCompatibleBitmap;
    decltype(&::SelectObject) selectObject = &::SelectObject;
    decltype(&::BitBlt) bitBlt = &::BitBlt;
    decltype(&::GetDIBits) getDIBits = &::GetDIBits;
};

// x/y are source-DC coordinates (virtual-screen pixels for the screen DC,
// negative left of or above the primary monitor). Returns a null image on
// any failure, with every GDI object it created released.
QImage readRect(HDC source, int x, int y, int w, int h, const Api& api = {});
} // namespace GdiReadback
