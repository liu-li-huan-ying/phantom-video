#include "ui/gdi_text.h"
#include <windows.h>
#include <cstring>

void GdiTextCache::init(SDL_Renderer* renderer) {
    renderer_ = renderer;
}

void GdiTextCache::shutdown() {
    for (auto& e : cache_) {
        if (e.tex) SDL_DestroyTexture(e.tex);
    }
    cache_.clear();
}

SDL_Texture* GdiTextCache::renderText(const std::string& utf8, int ptSize,
                                       int r, int g, int b, int& outW, int& outH) {
    if (!renderer_ || utf8.empty()) return nullptr;

    // UTF-8 → UTF-16
    int wLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (wLen <= 0) return nullptr;
    std::vector<wchar_t> wbuf(wLen + 1);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wbuf.data(), wLen);
    wbuf[wLen] = 0;

    HDC hdc = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(hdc);
    HFONT hFont = CreateFontW(
        -MulDiv(ptSize, GetDeviceCaps(hdc, LOGPIXELSY), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(memDC, hFont);

    RECT rc{ 0, 0, 0, 0 };
    DrawTextW(memDC, wbuf.data(), -1, &rc, DT_CALCRECT | DT_LEFT | DT_SINGLELINE);
    outW = rc.right;
    outH = rc.bottom;
    if (outW <= 0 || outH <= 0) {
        SelectObject(memDC, oldFont);
        DeleteObject(hFont);
        DeleteDC(memDC);
        ReleaseDC(nullptr, hdc);
        return nullptr;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = outW;
    bi.bmiHeader.biHeight = -outH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(memDC, hBmp);

    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(r, g, b));
    DrawTextW(memDC, wbuf.data(), -1, &rc, DT_LEFT | DT_SINGLELINE);

    // 转 SDL_Texture
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(
        0, outW, outH, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surf) {
        SDL_LockSurface(surf);
        Uint32* dst = (Uint32*)surf->pixels;
        Uint32* src = (Uint32*)bits;
        for (int y = 0; y < outH; ++y) {
            for (int x = 0; x < outW; ++x) {
                Uint32 px = src[y * outW + x];
                Uint8 a = (px >> 24) & 0xFF;
                Uint8 sr = (px >> 16) & 0xFF;
                Uint8 sg = (px >> 8) & 0xFF;
                Uint8 sb = px & 0xFF;
                // 非黑像素 alpha=255，黑像素 alpha=0（透明）
                if (sr > 5 || sg > 5 || sb > 5) a = 255;
                else a = 0;
                dst[y * surf->w + x] = (a << 24) | (sr << 16) | (sg << 8) | sb;
            }
        }
        SDL_UnlockSurface(surf);
    }

    SelectObject(memDC, oldBmp);
    DeleteObject(hBmp);
    SelectObject(memDC, oldFont);
    DeleteObject(hFont);
    DeleteDC(memDC);
    ReleaseDC(nullptr, hdc);

    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
    SDL_FreeSurface(surf);
    return tex;
}

void GdiTextCache::drawText(int x, int y, const std::string& utf8, int ptSize,
                             int r, int g, int b) {
    // 查找缓存
    std::string key = utf8 + "|" + std::to_string(ptSize) + "|" +
                      std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b);
    for (auto& e : cache_) {
        if (e.key == key && e.tex) {
            SDL_Rect dst{ x, y, e.w, e.h };
            SDL_RenderCopy(renderer_, e.tex, nullptr, &dst);
            return;
        }
    }

    // 渲染新文本
    int w = 0, h = 0;
    SDL_Texture* tex = renderText(utf8, ptSize, r, g, b, w, h);
    if (!tex) return;

    cache_.push_back({ tex, w, h, key });
    // 限制缓存大小
    if (cache_.size() > 200) {
        SDL_DestroyTexture(cache_.front().tex);
        cache_.erase(cache_.begin());
    }

    SDL_Rect dst{ x, y, w, h };
    SDL_RenderCopy(renderer_, tex, nullptr, &dst);
}
