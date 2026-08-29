#pragma once
// UpdateLayeredWindow (ULW) alpha compositing context.
// Handles DIB allocation, premultiplied alpha conversion, and ULW present.
// Extracted from main.cpp to reduce monolithic file size.

#include <windows.h>
#include <SDL.h>
#include <vector>
#include "core/logger.h"

// DIB memory context for ULW
struct UlwCtx {
    HDC memDC = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;
};

// Release DIB resources
inline void ulwDestroy(UlwCtx& ctx) {
    if (ctx.dib) { DeleteObject(ctx.dib); ctx.dib = nullptr; }
    if (ctx.memDC) { DeleteDC(ctx.memDC); ctx.memDC = nullptr; }
    ctx.bits = nullptr; ctx.w = ctx.h = 0;
}

// Allocate DIB for given dimensions
inline bool ulwResize(UlwCtx& ctx, int w, int h) {
    ulwDestroy(ctx);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    ctx.dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &ctx.bits, nullptr, 0);
    if (!ctx.dib) { LOG_ERROR("MAIN", "CreateDIBSection failed"); return false; }
    ctx.memDC = CreateCompatibleDC(nullptr);
    if (!ctx.memDC) { LOG_ERROR("MAIN", "CreateCompatibleDC failed"); return false; }
    SelectObject(ctx.memDC, ctx.dib);
    ctx.w = w; ctx.h = h;
    return true;
}

// Premultiply alpha and present via UpdateLayeredWindow
// srcPx: ARGB8888 pixel buffer from SDL_RenderReadPixels
inline void ulwPresent(UlwCtx& ctx, HWND overlayHwnd, HWND parentHwnd,
                       const Uint32* srcPx, int w, int h) {
    if (!overlayHwnd || !parentHwnd || !srcPx || w <= 0 || h <= 0) return;

    if (ctx.w != w || ctx.h != h) {
        if (!ulwResize(ctx, w, h)) return;
    }

    // Premultiply alpha (ULW requires premultiplied BGRA)
    Uint32* dst = (Uint32*)ctx.bits;
    const size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i) {
        Uint32 p = srcPx[i];
        Uint32 a = p >> 24;
        if (a == 0)        dst[i] = 0;
        else if (a == 255) dst[i] = p | 0xFF000000u;
        else {
            Uint32 r = ((p >> 16) & 255) * a / 255;
            Uint32 g = ((p >> 8) & 255) * a / 255;
            Uint32 b = (p & 255) * a / 255;
            dst[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    // ULW present (position follows parent client area origin)
    POINT pt = {0, 0};
    ClientToScreen(parentHwnd, &pt);
    POINT srcPt = {0, 0};
    SIZE sz = {w, h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC scr = GetDC(nullptr);
    BOOL ok = UpdateLayeredWindow(overlayHwnd, scr, &pt, &sz,
                                  ctx.memDC, &srcPt, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, scr);
    if (!ok) {
        static bool warned = false;
        if (!warned) { LOG_ERROR("MAIN", "UpdateLayeredWindow failed err=%lu", GetLastError()); warned = true; }
    }
}
